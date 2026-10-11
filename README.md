# Railway Reservation System – OS Synchronization (v3)

A C / POSIX-threads booking engine with **three websites** (Railway Portal + two partner sites) that demonstrates core Operating System synchronization concepts on a realistic problem: group berth allotment, cancellation refunds, and priority handling of partner bookings.

## What is new in v3
1. **Real train search** – choose **starting station**, **destination** and **date**, press *Search trains*, and the site lists every train that runs between those two stations (15 stations, 18 trains incl. return trains) with departure/arrival time, duration, distance, class-wise fare and free berths. Booking opens up to 60 days ahead; trains that already left today are shown as *Departed*.
2. **Booking flow (5 steps)** – Search → Choose train + class → **Passengers & berth preference** (names, 1–6 passengers, lower/middle/upper/side) → **Seats & meals** → Ticket. Same flow on the Railway Portal, MakeMyTrip and Goibibo.
3. **Seat-wise meals** – after the engine allots seat numbers, the customer sees every seat (e.g. `B1/4 – Anu Priya`) and picks a meal *for that seat* (Veg, Non-Veg, Jain, Snack box) or leaves **No meal**. A **"Skip meals & book ticket"** button books the ticket straight away with no meals. Meals are served at the seat; the main portal has a **Meals (Pantry)** tab: a catering manifest sorted by seat number.
4. **Seat hold with timeout (new OS idea)** – when the customer reaches the meal step the seats are *held* for 180 s (status HELD). Confirming turns the hold into a ticket; cancelling, starting a new search, closing the page or doing nothing releases the seats (a **reaper thread** frees expired holds). Held seats count as taken in the seat audit, so nobody else can be given them.
5. **Every train + date has its own berth map** (created on first booking). Fares depend on distance: `km x rate per 100 km` (1AC 150, 2AC 90, 3AC 60) with a minimum fare. Refund rules are unchanged; meals are refunded in full if you cancel 4 h or more before departure.

Note: a berth is reserved for the **whole run** of the train (not re-sold for the free part of the route) to keep the synchronization logic easy to explain.

## What is new in v2
1. **Berth allotment for families / groups (1–6 passengers)** – seats are auto-allotted at ticket time; the customer only chooses a *berth preference*.
   - 1AC: single berth (cabins of 4) · 2AC: lower, upper, side lower, side upper · 3AC: lower, middle, upper, side lower, side upper
   - A group is seated **together** (same compartment, or two neighbouring ones) whenever possible, using as many preferred-type berths as possible.
   - If no such block exists, passengers get separate berths in different compartments and the ticket says **"SEATS NOT AVAILABLE TOGETHER"**.
2. **Cancellation with refund** – time-based refund slabs (see below); berths return to the pool.
3. **Partner websites** – *MakeMyTrip* (10 % discount) and *Goibibo* (12 % discount), simulated. Their bookings are written to the **main Railway Portal database**, show up instantly on the main site ("All Bookings" tab, with a source badge) and are served with **higher priority**.

## OS concepts covered
| Concept | Where |
|---|---|
| Threads | one thread per web request/passenger (producers), 4 booking workers (consumers) |
| Mutex / mutual exclusion | `db_lock` write-lock around the critical section *choose berths + commit*; `q_mutex`, `log_mutex` |
| Readers–writers lock | `pthread_rwlock_t db_lock`: many website readers in parallel, one writer at a time |
| Counting semaphores | `q_slots` / `q_items` (bounded buffer), per-job `done` semaphore |
| Background thread + timeout | `reaper_thread` releases seat holds that are not confirmed within 180 s |
| Producer–Consumer | requests → bounded priority queue → workers |
| Priority scheduling + aging | partner = priority 1, direct = priority 2; after 3 partner jobs one waiting direct job is served (no starvation) |
| Critical section / race condition | `--unsafe` removes the lock → same berth sold twice; seat audit proves it |
| Deadlock avoidance | locks never nested in conflicting order |

## Architecture
```
Railway Portal (/)      MakeMyTrip (/makemytrip)      Goibibo (/goibibo)
        \                       |                        /
         \______  HTTP  (one thread per request)  ______/
                              |
                 Bounded PRIORITY queue (semaphores + mutex)
                  partner = high priority, direct = normal, aging
                              |
                    Booking worker threads (x4)
                              |
              write-lock (mutex) -> CRITICAL SECTION
                 choose berths -> commit -> create ticket
                              |
          Shared database: berth map + all bookings (every channel)
                              |
        Main portal reads it (read-lock): "All bookings", availability
```

## Build & run (Linux / WSL)
```bash
gcc main.c -o railway -pthread
./railway serve                      # websites on http://localhost:8080
./railway simulate                   # console stress test, safe mode (all requests fight for train 12345, MAS→NDLS)
./railway simulate --unsafe          # console race-condition demo
```
Open in a browser:
- Main website: http://localhost:8080/
- MakeMyTrip: http://localhost:8080/makemytrip
- Goibibo: http://localhost:8080/goibibo

Options: `--port N  --workers N  --requests N  --delay-ms N  --unsafe  --quiet  --web DIR`
Run the program from the project folder (it serves the `web/` folder).
- **Windows:** see `run_windows_wsl.txt`.  **macOS + VS Code:** see `run_mac_vscode.txt` (build task: Cmd+Shift+B).
- The semaphores are built from a mutex + condition variable (`PSem`), so the code runs on Linux, WSL and macOS (macOS has no `sem_init`).

## Demo script (5 minutes)
0. Open **MakeMyTrip** → pick *Chennai Central → New Delhi*, a date, *Search trains* → choose 3AC → enter names → on the **Seats & meals** step choose a meal for some seats and leave others at "No meal" → *Confirm ticket* (or press **Skip meals & book ticket**). Then open the main portal → **Meals (Pantry)** to see the manifest by seat number.
1. Open **MakeMyTrip**, book a family of 5 (3AC, lower berth) → ticket shows seats together, 10 % discount, "Synced to Railway Portal database".
2. Open the **main Railway Portal → All Bookings**: the MakeMyTrip booking is there with a red *MakeMyTrip* badge and *High (partner)* priority.
3. Book from **Goibibo** (12 % off) and watch it appear on the main site.
4. **Priority Demo** tab: 6 direct + 6 partner requests at once; partner requests are served first (aging lets a direct one through every 3rd turn).
5. **Cancel / Refund**: cancel a ticket choosing "12 to 48 hours" → 75 % refunded; berths become free again.
6. Console: `./railway simulate --unsafe` (double bookings) then `./railway simulate` (0 double bookings).
7. To see **"seats not available together"**, fill a class (e.g. book many 2AC tickets), cancel a few spread-out tickets, then book a family of 5.

## Berth layout
| Class | Coaches | Compartment | Berths |
|---|---|---|---|
| 1AC | H1 | cabin of 4 single berths | 24 |
| 2AC | A1, A2 | 6 berths: LB, UB, LB, UB, SL, SU | 96 |
| 3AC | B1, B2, B3 | 8 berths: LB, MB, UB, LB, MB, UB, SL, SU | 216 |

Seat labels look like `B1/14` (coach B1, berth 14). Fare per passenger = distance x rate (1AC ₹1.50/km, 2AC ₹0.90/km, 3AC ₹0.60/km; minimum 1AC ₹450, 2AC ₹260, 3AC ₹190). Chennai → New Delhi (2180 km) is about ₹3270 / ₹1960 / ₹1310.

Meals: Veg ₹180, Non-Veg ₹220, Jain ₹190, Snack box ₹90 (no partner discount on meals).

Train numbers, timings and distances are **sample data** for the project, not the real railway timetable.

## Refund policy (hours before departure)
| Time left | Refund |
|---|---|
| 48 h or more | paid amount − flat fee per passenger (1AC ₹240, 2AC ₹200, 3AC ₹180) |
| 12 – 48 h | 75 % of paid amount |
| 4 – 12 h | 50 % of paid amount |
| under 4 h | no refund |

Departure is the real departure time of the chosen train/date; the cancel form also lets you pick a slab to demonstrate each case. Partner sites can cancel only their own tickets; the Railway Portal can cancel any ticket.

## HTTP API (used by all three websites)
| Endpoint | Purpose |
|---|---|
| `GET /api/meta` | stations, trains, meal menu, today's date, hold time |
| `GET /api/trains?from=MAS&to=NDLS&date=YYYY-MM-DD` | train search: times, fare and free berths per class |
| `POST /api/book` | `channel, key, train, from, to, date, class, pref, names, hold=1` → seats **held** (without `hold=1` the ticket is confirmed immediately) |
| `POST /api/confirm` | `channel, key, pnr, meals=0,1,2` (meal id per seat, `0`/empty = no meal) → ticket |
| `POST /api/release` | `channel, key, pnr` → give the held seats back |
| `GET /api/meals?train=12345&date=…` | pantry manifest: meal per seat number |
| `POST /api/cancel` | `channel, key, pnr, hours` → refund |
| `GET /api/bookings[?channel=MMT]` | read all bookings from the main database |
| `GET /api/availability?train=12345&date=…` | free berths per class and berth type for one train/date |
| `GET /api/stats`, `/api/log` | counters, seat audit, activity log |
| `POST /api/demo/burst`, `/api/reset` | priority demo, reset |

Partner API keys (simulation): `MMT-KEY-2026`, `GOI-KEY-2026`. A wrong key is rejected with HTTP 403.

## Files
- `main.c` – booking engine + HTTP server (all synchronization logic)
- `web/` – `index.html` (main), `makemytrip.html`, `goibibo.html`, `common.js` (search + booking wizard + meals), `style.css`
- `PROJECT_REPORT.md`, `presentation_outline.md`, `run_windows_wsl.txt`, `run_mac_vscode.txt`
- `.vscode/tasks.json` – one-click build and run tasks for VS Code
- `screenshots/` – sample screens
