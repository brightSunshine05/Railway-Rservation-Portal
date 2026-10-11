# Project Report: Railway Reservation System – OS Synchronization (v3)

## 1. Abstract
Many passengers book railway tickets at the same moment, through the railway's own website and through partner websites such as MakeMyTrip and Goibibo. Berths are a shared resource, so uncontrolled concurrent access causes race conditions (the same berth sold twice). This project builds a booking engine in C with POSIX threads that (a) allots berths to families and groups so they sit together whenever possible, (b) refunds money on cancellation, and (c) accepts bookings from partner websites at a discount, stores them in the main database, and serves them with higher priority. A mutex-protected critical section, semaphores, a bounded priority queue with aging, and a final seat audit guarantee correctness.

## 2. Objectives
- Allot seats together for a group of up to 6 passengers (same compartment / neighbouring compartments); otherwise allot separate seats and clearly print *"seats not available together"* on the ticket.
- Offer berth preferences per class: 1AC single; 2AC lower/upper/side lower/side upper; 3AC lower/middle/upper/side lower/side upper. Seat numbers are auto-allotted.
- Cancel tickets and refund part of the money by a time-based policy.
- Support partner websites with discounts; show their bookings on the main website; give them priority.
- Demonstrate threads, mutual exclusion, semaphores, producer–consumer, readers–writers, priority scheduling and aging, and show the race condition and its fix.

## 3. Problem Statement
Concurrent booking requests arrive from three websites. The system must guarantee that **(1)** a berth belongs to at most one active ticket, **(2)** groups are kept together when possible, **(3)** refunds follow the policy, **(4)** partner bookings are visible on the main site and served first, **(5)** no request waits forever (no starvation) and the system cannot deadlock.

## 4. System Design
```
Railway Portal   MakeMyTrip   Goibibo      (3 websites, HTTP)
       \              |           /
        one thread per request  (producers)
                      |
   Bounded PRIORITY queue: q_slots / q_items semaphores + q_mutex
         partner = priority 1, direct = priority 2, aging
                      |
         Booking worker threads (consumers, x4)
                      |
            write-lock (mutex) -> CRITICAL SECTION
              choose berths -> commit -> ticket
                      |
       Shared database (berth map + all bookings)
                      |
   Main website reads it with read-lock (parallel readers)
```

### 4.1 Channels
| Channel | Discount | Priority | Access |
|---|---|---|---|
| Railway Portal (direct) | 0 % | 2 (normal) | no key |
| MakeMyTrip (simulated) | 10 % | 1 (high) | API key `MMT-KEY-2026` |
| Goibibo (simulated) | 12 % | 1 (high) | API key `GOI-KEY-2026` |

A partner request without a valid key is rejected (HTTP 403). Every confirmed partner booking is stored in the same database as direct bookings, so the main website's *All Bookings* tab shows it immediately with its source and priority. Partner sites also read their own bookings back from this database ("My Bookings").

### 4.2 Coaches and berths
| Class | Coaches | Compartment | Total |
|---|---|---|---|
| 1AC | H1 | cabin of 4 single berths | 24 |
| 2AC | A1, A2 | 6 berths: LB, UB, LB, UB, SL, SU | 96 |
| 3AC | B1, B2, B3 | 8 berths: LB, MB, UB, LB, MB, UB, SL, SU | 216 |

### 4.3 Berth-allotment algorithm (`choose_seats`)
Input: class, preferred berth type, number of passengers *n* (1–6).
1. If fewer than *n* berths are free in the class, reject the booking.
2. **Together search.** For every coach and every pool made of one compartment or two neighbouring compartments, take the preferred-type berths first and then the lowest free berths until *n* are chosen. Rank candidates by: fewest compartments used, then most preferred-type berths, then smallest berth-number span. The best candidate is allotted and the ticket says *"Seats allotted together"*.
3. **Fallback.** If no pool has *n* free berths, scatter the passengers: first preferred-type berths in different compartments, then any berth in different compartments, then any free berth. The ticket says *"SEATS NOT AVAILABLE TOGETHER – passengers allotted separate berths in different compartments/coaches"*.

Example: a family of 5 in 3AC asking for lower berths gets `B1/1 (LB), B1/4 (LB), B1/2, B1/3, B1/5` – all in one compartment. A pair asking for upper berths in 2AC gets `A1/2, A1/4` (both upper). With free berths scattered one per compartment, a family of 5 gets `A2/3, A2/9, A2/15, A2/21, A2/27` and the "not together" note.

### 4.4 Cancellation and refund
| Time before departure | Refund |
|---|---|
| 48 h or more | paid − flat fee per passenger (1AC ₹240, 2AC ₹200, 3AC ₹180) |
| 12 – 48 h | 75 % of paid |
| 4 – 12 h | 50 % of paid |
| under 4 h | 0 |

Cancellation runs inside the same critical section, frees the berths, and records charge and refund on the ticket. Partner sites can cancel only tickets booked through them.

Example (3AC, family of 5, direct, paid ₹6250): 100 h → refund ₹5350; 30 h → ₹4688; 8 h → ₹3125; 2 h → ₹0. A partner booking is cheaper: the same family pays ₹5625 on MakeMyTrip (10 % off) and a 2AC family of 5 pays ₹8360 on Goibibo (12 % off instead of ₹9500).

## 5. Synchronization Mechanisms
| Mechanism | Purpose |
|---|---|
| `pthread_rwlock_t db_lock` (write mode) | Mutual exclusion for *choose berths + commit* and for cancellation (the critical section) |
| `db_lock` (read mode) | Website reads (bookings list, availability, stats) run in parallel, never see a half-written ticket |
| `PSem` (mutex + condition variable) | Portable counting semaphore used for the queue (macOS has no `sem_init`) |
| `q_slots` semaphore | Free places in the request queue; web threads block when it is full (back-pressure) |
| `q_items` semaphore | Number of queued requests; workers sleep when it is 0 |
| `q_mutex` | Protects the queue array and priority selection |
| per-request `done` semaphore | Web thread sleeps until a worker finishes its request |
| `log_mutex` | Keeps activity-log lines intact |
| atomic add (`__sync_fetch_and_add`) | PNR numbers and counters |

## 6. Priority Scheduling and Aging
Workers always pick the oldest **priority-1 (partner)** request first. To prevent starvation of direct customers, after every `AGING_LIMIT = 3` consecutive partner requests served while a direct request is waiting, the oldest direct request is served. Ordering inside a priority is FIFO by sequence number.

The *Priority Demo* sends 6 direct and 6 partner requests together while workers are held until the queue is full. Typical result: average service position **partner 8.0 vs direct 13.0**; the console simulation with 60 requests gave **partner 20.0 vs direct 41.0**.

## 7. Critical Section and Race Condition
The critical section is *check berths free → mark them owned → create the ticket*. Without the lock two workers can choose the same berths before either commits (a check-then-act race). A short simulated payment delay inside the section (`usleep(2000)`) makes the race repeatable.

| Mode | 60 simulated requests |
|---|---|
| Safe (`./railway simulate`) | Double-booked berths **0**, audit CORRECT |
| Unsafe (`./railway simulate --unsafe`) | Double-booked berths **12 – 19**, audit RACE CONDITION DETECTED |

The **seat audit** recounts every confirmed ticket's berths and reports double-booked berths and orphan berths (owned by no ticket).

## 8. Deadlock and Starvation Analysis
- **Deadlock:** a worker takes `q_mutex` only inside the queue functions and `db_lock` only inside the booking/cancel functions; the two are never held together, so no circular wait exists. Web threads wait on `q_slots` / `done` but hold no lock.
- **Starvation:** FIFO inside each priority plus aging guarantees every direct request is served; the bounded queue stops unlimited growth.

## 9. Testing and Results
| Test | Result |
|---|---|
| Family of 5, 3AC, lower pref | `B1/1, B1/4` lower + 3 neighbours, together |
| Pair, 2AC, upper pref | `A1/2, A1/4` both upper |
| 6 passengers, 1AC | `H1/1 … H1/6`, together |
| Scattered free berths, family of 5 | allotted separately, ticket shows "not available together" |
| Partner booking | stored with channel, discount and priority; visible on main website |
| Wrong partner key | HTTP 403 |
| Middle berth in 2AC | rejected (berth type does not exist) |
| Refund slabs | ₹5350 / ₹4688 / ₹3125 / ₹0 |
| Partner cancels a direct ticket | refused |
| Browser test (3 sites, booking, cancel, burst) | no JavaScript errors |

## 10. Conclusion
A single mutex-protected critical section, semaphores-based bounded priority queue and a readers–writers lock let several websites book, cancel and read concurrently without selling a berth twice. Group allotment keeps families together when it can and tells the customer clearly when it cannot, partner sites get discounts and priority without starving direct customers, and refunds follow a clear time-based policy.

## 10a. Version 3 additions: train search, dates and seat-wise meals
- **Data model:** `Train` (route with stops, km, running minutes, classes offered), `Journey` (= train + calendar date, owns its own berth map, created lazily under `jmutex`), `Trip` (boarding station, destination, date → departure/arrival time, distance, fare per class).
- **Search:** `GET /api/trains` evaluates every train, keeps those whose route contains *from* before *to*, and reads free berths of that day's journey under the **read lock** (many searches run in parallel).
- **Two-phase booking:** `JOB_BOOK(hold)` picks and reserves seats inside the write-lock critical section (status HELD, 180 s); `JOB_CONFIRM` adds the meal chosen for each seat number and makes the ticket CONFIRMED; `JOB_RELEASE` returns the seats. All three go through the same bounded priority queue and worker threads, so partner priority and aging apply to them too.
- **Hold timeout:** `reaper_thread` wakes every second, takes the write lock and frees every HELD booking whose time ran out. The seat audit treats HELD seats as owned, so a held berth can never be sold twice.
- **Pantry manifest:** confirmed bookings of one journey are sorted by (class, coach, berth) so catering staff know which meal goes to which seat.
- **Refunds:** ticket part follows the slab table; the meal part is refunded in full when cancelling 4 h or more before departure, otherwise not.
- **Simplification:** a berth is booked for the whole run of the train (no re-selling of a free part of the route).

## 11. Future Enhancements
- Partial cancellation (cancel one passenger of a group), waiting list and RAC.
- Segment-wise berth reuse (sell the same berth for different parts of a route); per-berth locks for higher concurrency.
- Real partner integration over a separate server with signed requests; persistence in a database file.
- Payment gateway simulation, seat-map picker, age/gender rules (lower berth for seniors).

## 12. References
- Silberschatz, Galvin, Gagne – *Operating System Concepts*
- POSIX Threads Programming (`pthread.h`, `semaphore.h`) man pages
