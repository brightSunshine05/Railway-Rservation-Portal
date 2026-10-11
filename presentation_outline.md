# Presentation Outline (v3 – 17 slides)

1. **Title** – Railway Reservation System: OS Synchronization (team, faculty Dr. Generaldine Bessie Amali D)
2. **Abstract** – shared berths, 3 websites, group allotment, refund, partner priority
3. **Why we chose this topic** – real-world, covers the OS syllabus, visible bug, hands-on C
4. **Problem statement** – double booking, groups split, partner bookings lost / not prioritised
5. **Ideas used and why** – threads, mutex, rwlock, semaphores, priority queue + aging, audit
6. **System architecture** – 3 websites → priority queue → workers → critical section → shared database
7. **Berth allotment (faculty requirement 1)** – class options, layout, together vs not together
8. **Allotment algorithm + examples** – family of 5, pair upper, "seats not available together"
9. **Cancellation and refund** – slab table, berths released
10. **Partner websites (faculty requirement 2)** – MakeMyTrip 10 %, Goibibo 12 %, API key, data stored in main DB
11. **Priority scheduling + aging** – partner first, 1 direct after every 3 partner, no starvation
12. **Producer-consumer queue and critical section** – semaphores, mutex, race vs safe
13. **Results** – safe 0 double bookings vs unsafe 12–19, priority 8.0 vs 13.0, refund numbers
14. **Work division** (3 members)
15. **Conclusion and future scope**
16. **Thank you**

## Extra slide for v3: Search, seat hold and meals
- Station + date search, 18 trains, own berth map per train per date.
- Two-phase booking: HOLD (180 s, reaper thread) → choose meal per seat or skip → CONFIRM.
- Meals manifest by seat number for the pantry car.

## Live Demo Script
```
./railway serve
  1. http://localhost:8080/makemytrip  -> search MAS to NDLS + date -> 3AC -> names -> pick meals per seat (or skip) -> ticket + "Synced"
 2. http://localhost:8080/            -> All Bookings tab: booking appears (MakeMyTrip, High priority)
 3. http://localhost:8080/goibibo     -> book, show 12 % discount
 4. Main site -> Priority Demo        -> partner served first
 5. Main site -> Cancel / Refund      -> 12-48 h slab -> 75 % refund
Ctrl+C, then:
./railway simulate --unsafe           -> RACE CONDITION DETECTED
./railway simulate                    -> CORRECT
```

## Suggested Work Division
- **Part 1 – Deepansh:** HTTP server + the three websites (web/ folder), request queue and semaphores, API.
- **Part 2 – Mahasweta:** berth-allotment algorithm, critical section, mutex / rwlock, cancellation and refund, `--unsafe` demo.
- **Part 3 – Xinyi:** partner channels (discount, API keys, priority + aging), statistics and seat audit, testing, report, README.

## Likely Viva Questions
- How do you keep a family together? What if you cannot? (together search, fallback, ticket note)
- Why a readers-writers lock instead of a plain mutex?
- How is priority implemented and how do you avoid starvation? (priority queue, aging)
- Where exactly is the critical section? What happens without the lock?
- Why can two locks here not deadlock?
- Mutex vs binary semaphore? Why do you need both semaphores and a mutex for the queue?
- What happens if a customer holds seats and never confirms? (reaper thread frees them after 180 s; audit counts HELD seats so no double sale)
- Why do hold, confirm and release also go through the priority queue and write lock? (all changes to the shared database are serialized the same way)
