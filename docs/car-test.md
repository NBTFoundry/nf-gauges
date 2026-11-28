<!-- SPDX-License-Identifier: MPL-2.0 -->
# Car test: Gauges

Packages: `gauges-0.0.9.nbt` (install), `gauges-0.0.10.nbt` (update),
`gauges-0.0.9-SHA256SUMS.txt`; the NBT Manager test kit dev138 or later
(diagnostics, bridge 1.8; an older manager refuses them). A Gauges from Get apps (0.1.0) is newer: uninstall it first. Parked, about 30
minutes, phone only, nothing in the OBD port.

Main question: does the gateway answer NBT Manager's diagnostics on the head
unit's own link? The CONNECTION page shows it.

## A. Install, health check, settings

| # | Do | Look for |
| --- | --- | --- |
| A0 | Phone: Library › open `gauges-0.0.9.nbt` | **Project: NBTFoundry nf-gauges**, **Licence: MPL-2.0; LVGL MIT; Montserrat OFL-1.1**, then the text ending with the repository's address |
| A1 | Phone: Car › install Gauges; approve on the car | The review has **no** "Can clear fault codes" line (read-only); the health check passes (about 10 s), "Installed" |
| A2 | Car: hold the push on **gauges** › settings | 6 settings: Data source, Units, Lambda as, Boost gauge top, Drivetrain, Debug log |
| A3 | Set **Debug log: on** | Kept when you leave and come back |

## B. Ignition on, engine off

| # | Do | Look for |
| --- | --- | --- |
| B1 | Start **gauges** | The ENGINE page: three round gauges, a status at the top right |
| B2 | Turn the controller to the last page, **CONNECTION**; wait 20 s. **Take a photo** | The lines DME, GATEWAY, DME INDEX, STANDARD VALUES, LAST PROBLEM, SEARCHED, IDRIVE CAR DATA |
| B3 | Read the status at the top right | One of: **DME nn HZ** (green: the DME answers), **IDRIVE CAR DATA** (amber), **SEARCHING** (no gateway answered), **CONNECTING** (a gateway, but no DME yet). GATEWAY on CONNECTION: **NBT Manager** (through its diagnostics); LAST PROBLEM says why when it waits |
| B4 | If **DME**: the TEMPERATURES page | Coolant and oil close to the car's own (oil may be `--` if the DME does not give it) |
| B5 | The fourth page | A petrol car: **IGNITION** (timing gauge). A diesel: **DPF** (soot load gauge, REGENERATION, PRESSURE DROP, FILTER INLET/OUTLET...). Note which, and **take a photo** if DPF |
| B6 | The fifth page, **TRANSMISSION** | The car drawn from above; GEAR shows **P** (and the drive mode, e.g. COMFORT); wheel speeds `--` and a note about definition files (none installed); DRIVE: SET IT IN SETTINGS (Drivetrain: auto, no definition file); REAR-WHEEL DRIVE with Drivetrain: rwd |
| B7 | Foot on the brake, the selector to **R**, **N**, **D**, back to **P** | GEAR follows: R, N, D1 (or S1 in Sport), P. If a letter is wrong, note what the selector was on |

## C. Engine running (parked)

| # | Do | Look for |
| --- | --- | --- |
| C1 | Start the engine; ENGINE page | Boost below zero at idle (about −0.6 bar), lambda about 1.00, timing a few degrees |
| C2 | Blip the throttle to 3000 rpm | Boost and timing move at once, with the needle following smoothly; FUEL & AIR: rpm and rail pressure follow |
| C3 | Push the controller | The peaks (PEAK, MAX) reset |
| C4 | Hold the push › settings › **Data source: cardata**; back to Gauges | Status **IDRIVE CAR DATA**; rpm on FUEL & AIR, coolant and oil on TEMPERATURES; boost, lambda and timing `--` |
| C5 | **Data source: demo** | Status **DEMO**: the gauges sweep through a simulated drive |
| C6 | **Data source: auto**, **Units: imperial**, **Lambda as: afr**, **Boost gauge top: 3bar** | psi and °F, lambda as about 14.7, the boost gauge to 45 psi |
| C7 | TRANSMISSION page; turn the steering wheel to the **left**, about half a turn, then right | STEERING follows (**L** to the left), the drawn front wheels turn the same way; **take a photo** at full left. If L/R or the wheels go the wrong way, say so |
| C8 | Still on TRANSMISSION, in P or N: blip the throttle to 3000 rpm and up to 4500 | TORQUE & POWER: two curves appear between idle and the highest rpm reached (low, no load), a line and its two dots follow the engine (the line alone at no load). Drive mode switch: the mode text follows (SPORT, COMFORT...) |
| C9 | Settings › **Drivetrain: xdrive** (then back to **auto**) | The front shaft and axle appear on the car; DRIVE shows FRONT/REAR `--` without a definition file |
| C10 | Turn through all seven pages, BACK | Each page draws without flicker; BACK closes Gauges |

## D. Update

| # | Do | Look for |
| --- | --- | --- |
| D1 | Phone: update to `gauges-0.0.10.nbt`; approve | Health check passes; settings and the last page shown are kept |

## What to send back

- Photos: CONNECTION (B2), DPF if shown (B5), TRANSMISSION steering left (C7);
  the status in B3; the gear letters in B7.
- The log before switching off: phone › **gauges** › **Show log** › Share
  (the `dme:` and `ui:` lines). If it stayed SEARCHING: Car › Manage ›
  **iDrive log** too.

## Going back

Uninstall **gauges** from its page on the phone (or hold › Uninstall on the
car).
