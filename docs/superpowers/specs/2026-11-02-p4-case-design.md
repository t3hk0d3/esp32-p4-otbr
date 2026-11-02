# 3D-printable case for the ESP32-P4-WIFI6-POE-ETH board: design

Date: 2026-11-02. Status: draft for review.

## 1. Purpose and scope

The border router runs on a bare Waveshare ESP32-P4-WIFI6-POE-ETH board. This adds a 3D-printable case for it. The case
hangs on a wall from two screws (cable end down, or SMA side up) or lies on a table. In daily use the only cable is PoE
Ethernet, but these stay reachable without opening the case:

- every edge connector (USB-C, USB-A, RJ45);
- the microSD slot;
- the BOOT and RESET buttons;
- an external Wi-Fi antenna (SMA bulkhead);
- wiring on the 40-pin header.

In scope:

- a parametric case model generated from code;
- STL and STEP outputs;
- a fit check against the vendor's 3D model of the board;
- a docs page on printing and assembly.

Out of scope:

- openings for the camera and display FPC sockets, the two 4-pin 1 mm sockets, the 2-pin SH1.0 socket and the underside
  2-pin battery socket (all stay covered);
- vents under the board;
- legs or feet;
- pry cut-outs for the lid (deferred, see section 10);
- light pipes;
- any change to the firmware.

Anti-goals: the build fails if

- a connector can't be fully plugged in;
- a button can be pressed by a finger;
- either part needs supports to print;
- the board can't be fitted or removed without force;
- something wider than 3.5 mm can fall into the case through the lid or the header slot.

## 2. Inputs

- **Board model:** `https://files.waveshare.com/wiki/ESP32-P4-WIFI6-POE-ETH/ESP32-P4-WIFI6-POE-ETH-3D.zip`
  - sha256 of the zip: `916430173b5a375567025b9757aa6bb19cf380d75236a5d6fbf0c66650d78e11`
  - The zip holds a STEP file (`ESP32-P4-WIFI6-POE-ETH_3d-20251213.stp`, sha256
    `41e0f374133963061dc032690541756e3c6f774ba3b4af83e3d065366f9a6313`), a DWG and a dimensioned PDF.
  - The zip is about 5 MB and the STEP is 28 MB, so neither is committed. A script fetches them and checks the hashes.
- **Sketches agreed during brainstorming:**
  - `docs/images/case/v5-views.svg`: front, both ends, both sides, back.
  - `docs/images/case/v5-detail.svg`: section through the cable-end corner tabs, and a close-up of a tab bump.
  - `docs/images/case/v4-lid.svg`: the grille patterns that were compared; honeycomb was chosen. The wings on that
    sheet are superseded by the inside tabs.
  - `case/sketches/` holds the throwaway scripts that draw the sketches. They are not the model.
  - The sketches are approximate. Where they differ from the numbers here, this document wins.

## 3. Board facts (measured from the STEP model)

All coordinates use the STEP model's own frame, which this document calls the *board frame*:

- **X** runs 0 to 55 across the short side. X = 0 is the button edge; X = 55 is the SMA side.
- **Y** runs 0 to 68 along the long side. Y = 0 is the cable edge; Y = 68 is the header edge.
- **Z** = 0 is the top face of the PCB. The PCB spans Z −1.6 to 0.

The builder re-derives every number below from the model in code, not by copying it. The figures here are for checking
the result.

| Item | Position (board frame, mm) | Notes |
| --- | --- | --- |
| PCB | X 0–55, Y 0–68, Z −1.6–0, corner radius 2 | |
| Mounting holes | Ø3 at (3.5, 24.3), (51.5, 24.3), (3.5, 57.3), (51.5, 57.3) | |
| USB stack (`USB_TPYPE-000`) | X 4.26–19.93, Y −1.70–15.76, Z up to 15.99 | USB-C at the bottom (about Z 1–4.6), USB-A above it; faces the −Y direction; tallest part |
| RJ45 (`RJ45_HR911105A`) | X 21.86–37.90, Y −1.99–19.41, Z up to 13.30 | PoE input; faces the −Y direction |
| microSD socket (`SD1_ASM`) | X 39.63–50.98, Y 1.23–12.48, Z 0–1.32 | push-push; the card goes in from the −Y direction |
| Buttons (`SWITCHER-4_5X3_3XH3_4`, ×2) | X −0.35–3.65; Y 3.35–7.95 and 10.05–14.65; Z −0.5–3.3 | side-actuated; plungers face the −X direction; which is BOOT comes from the model or silkscreen |
| LED (`LED1_ASM`) | X 0.30–1.62, Y 17.35–19.10, Z 0–0.5 | top-emitting 0603; visible through the grille |
| 40-pin header | X 2.10–52.90, Y 61.46–66.54, Z up to 8.30 | pins also stick out 3 mm below the board |
| Big capacitors | about X 47–54, Y 30–45, Z up to 7.8 | the reason the SMA sits at Y ≥ 50 |
| PoE transformer (`T1_ASM`) | X 33.75–47.05, Y 27.65–44.42, Z up to 12.0 | the main heat source |
| Antenna socket | about (30.3, 26.6), on the radio module | IPEX/u.FL type; the user's pigtail is about 8 cm long |
| Underside | deepest part 3.3 below the PCB bottom (2-pin socket near (28, 57)) | the rest mostly under 2.3 |

## 4. Decisions

| Topic | Decision |
| --- | --- |
| Parts | two printed parts: **base** and **lid** |
| Base | one part: flat back plate, all four walls, four board standoffs |
| Lid | flat honeycomb grille plate with four tabs; sits flush in a rebate on the wall tops |
| Lid fixing | four corner tabs hang into the inner corners of the base and guide the lid into place; a bump on each, ramped 45° on both sides, clicks into a dent in the side wall: push to close, lever up to open |
| Opening the lid | lever it up through the grille with a small flat screwdriver; no pry cut-outs in this revision |
| Board fixing | four M2.5 countersunk (flat-head) screws from the back: through a countersunk hole in the back plate, up through a Ø6 × 5 mm standoff and the board's hole, held by an M2.5 nut on top of the board; the heads sit flush with the back, so the case still lies flat on the wall |
| Board fitting | the board goes in from the open top, tilted, connectors first into the cable-wall holes, then the header end is lowered onto the standoffs |
| SMA | D-hole in the right (X = 55) wall; fitted after the board: the pigtail clips onto the board, the bulkhead goes out through the wall from inside, nut tightened outside |
| 40-pin header | wiring (jumpers or a ribbon cable) leaves through a slot along the top of the header-end wall; no opening in the lid over the header |
| Wall mounting | two L-shaped keyholes in the flat back plate, 32 mm apart, for either orientation |
| Table | lies on its back; stick-on rubber feet optional, not modelled |
| Cooling | honeycomb lid (about 60% open over the grille area) plus the header-end slot; no vents in the back plate or side walls |
| Buttons | Ø2.5 pin-holes in the left wall on the plunger axes; a finger can't reach the plunger |
| Wall / plate thickness | 2.0 mm; lid plate 2.0 mm |
| Clearance board ↔ inner wall | 0.5 mm, with a local recess at the buttons (see 5.2) |
| Clearance over the tallest part | 1.0 mm (lid underside at board Z 17.0); the extra 0.5 mm leaves 1 mm of wall above the USB opening |
| Outer size | about 60 × 73 × 27.6 mm |
| Printing | FDM, 0.4 mm nozzle, PETG recommended (the tabs flex), PLA acceptable; no supports; base prints back-plate down, lid prints top-face down |

## 5. Geometry

Heights are given in *case Z*. Case Z = 0 is the outer back face, and the board frame's Z = 0 sits at case Z 8.6.

| Layer | Case Z (mm) |
| --- | --- |
| Back plate | 0–2 |
| Standoffs | 2–7 |
| PCB | 7–8.6 |
| Tallest part (USB stack) | up to 24.6 |
| Lid underside (body) | 25.6 |
| Rebate floor (lid flange rests here) | 26.6 |
| Wall tops and lid top (flush) | 27.6 |

In plan view, the inner outline is the PCB outline plus 0.5 mm all round, and the outer outline adds 2 mm of wall. The
outer vertical edges are rounded.

### 5.1 Base: back plate and keyholes

- The plate is flat and solid apart from the keyholes.
- There are two keyholes: K1 under board (X 10, Y 42) and K2 under board (X 42, Y 42).
- Each has:
  - an entry hole of Ø8;
  - two arms, 4.3 mm wide and 7 mm long:
    - one toward +Y, used when the case hangs cable end down (the screws are level);
    - one toward +X, used when it hangs SMA side up (the same screws, one above the other).
- Sized for 3.5–4 mm screws with pan heads of Ø7.5 or less.
- The head sits inside the case, under the board, and stands at most 2.5 mm proud of the plate's inner face. The docs ask
  for pan-head screws with heads of 2.5 mm or less, with the screw left 2 mm proud of the wall. The free height under the
  board is about 5 mm at K1 and 3.0–3.6 mm at K2.
- If the K2 check fails, both keyholes may move together, keeping 32 mm spacing and the same Y. If that fails too, the
  spacing may shrink to 25 mm or more. Record the final positions in the docs.
- Nothing else (standoff, rib) may sit inside the area each L sweeps plus the head radius.

### 5.1a Base: board screw holes

- Under each of the four standoffs, a through-hole runs from the outer back face up through the standoff, on the axis
  of the board's mounting hole: Ø2.8 clearance for M2.5.
- At the back face it has a 90° countersink for an ISO 10642 / DIN 7991 M2.5 head (about Ø5 at the surface), sunk
  0.2 mm below flush so the head never stands proud against the wall.
- Screw length: 2 mm plate + 5 mm standoff + 1.6 mm board + 2 mm nut + a thread or two, so M2.5 × 12. The docs give it.
- On top of the board, each nut (5 mm across flats, 2 mm tall) needs a free circle of about Ø6.5 around the hole. The
  builder checks this against the top of the board model; the nearest parts are the FPC socket near (3.5, 57.3) and the
  large capacitor near (51.5, 24.3).
- The standoffs' top faces stay flat rings that the board rests on.

### 5.2 Base: walls

All four walls run full height, to case Z 27.6. A rebate on the inside of their top edge takes the lid: 1 mm deep (lid
top flush) and half the wall thickness wide. The lid is stepped: a 1 mm flange sits in the rebate and a 1 mm body
sits inside the walls (2 mm total), so its underside is at case Z 25.6.

- **Cable wall (Y = 0 edge):**
  - **Openings:** one for the USB stack (USB-C and USB-A together), one for the RJ45 and one slot for the microSD.
    - Each is the connector envelope plus 0.5 mm, except the top edge of the USB and RJ45 openings, which gets 1 mm so
      the board can tilt in.
    - The USB opening's top edge (case Z ≈ 25.6) leaves 1 mm of wall below the rebate floor (26.6).
  - **microSD:** the slot runs from 0.6 mm below the board's top face to 2 mm above it, across X 39.2–51.4. It has a
    thumb scoop in the outer face below it, so a fingernail or pencil can push the card in and out.
  - The connector faces end up 0.5–0.8 mm behind the outer face.
  - The tops of these openings print as bridges up to about 17 mm. That's accepted (see section 8).
- **Header-end wall (Y = 68 edge):**
  - A cable slot 54 mm wide (X 0.5–54.5), cut from case Z 22.1 up through the top of the wall so it needs no bridge.
    The lid closes it from above. The narrowest point of the path into the case is at the wall's inner face, between
    the slot floor and the lid body's underside: 3.5 mm (case Z 22.1–25.6). Within the wall's thickness it is taller,
    up to the flange.
  - 3.5 mm keeps to the "nothing wider than 3.5 mm falls in" rule when this end faces up on the wall. Ribbon cable
    (about 1.3 mm) and Dupont housings (2.5 mm) pass.
  - A real 2×20 IDC ribbon plug (about 55.9 × 9 × 9 mm, seated on the header body) must fit with 0.5 mm clearance. The
    inner box is only 56 mm wide and ends 2 mm past the header, so the walls get local 1 mm-deep recesses on their
    inner faces around the header end: both side walls over board Y 58–68.5, and the header-end wall itself, from the
    board's top face up to the slot. The walls are 1 mm thick there.
- **Left wall (X = 0 edge, buttons):**
  - Two Ø2.5 pin-holes on the plunger axes (Y 5.65 and 12.35 and the plunger's Z, taken from the model).
  - The plungers sit only 0.15 mm from the inner face, so the inner face gets a shallow local recess around both buttons
    to keep ≥ 0.3 mm clearance and let the board tilt in. There are no guide tubes.
- **Right wall (X = 55 edge, SMA):**
  - A D-shaped hole, Ø6.5 with a flat at 5.9, at board Y ≈ 52 and case Z ≈ 17.6.
  - The builder checks clearance inside: a 5 mm radius around the axis, out to 12 mm from the wall, must miss the board,
    the capacitors and the lid tabs.
- **Tab dents:** each side wall has two dents in its inner face, one near each end, on the axis of the corner tabs' side
  legs (5.3). Their upper and lower faces match the bump's 30° ramps (60° from horizontal), so they print without supports. At the header end the dent sits
  above the ribbon-plug recess, where the wall is full thickness.

### 5.3 Lid

- **Plate:** 2 mm thick, fits the rebate with 0.2 mm clearance per side.
- **Frame:** solid, 5.5 mm wide, all round.
- **Grille:**
  - Honeycomb inside the frame, holes 3.5 mm across flats, 1 mm walls (about 60% open).
  - One cross rib about 1.8 mm wide across the middle (around board Y 34) stiffens it.
  - The grille sits flat on the build plate when printing, so it has no bridges.
- **Corner tabs (snaps and guide pins):**
  - Four identical tabs, hanging from the frame's underside into the four inner corners of the base, against the side
    walls. Each is 10 mm long along the side wall and reaches 3.5 mm below the lid's underside, to about 13.5 mm above the
    board.
  - **Built for layer strength:** the lid prints top-face down, so each tab's layers stack along its length and the layer
    bonds take the bending load. To keep that load low and spread out:
    - the tabs are thick: 2.4 mm at the root;
    - a 1.5 mm fillet joins each tab to the lid frame, so there's no sharp corner for a crack to start from;
    - they're 10 mm wide;
    - the snap is shallow, so the tab barely bends (see the bump below);
    - `docs/case.md` recommends PETG and solid infill for the lid.
  - As the lid goes down they find the corners first and steer it into the rebate. Each tab's end sits 0.2 mm from the end
    wall, so the four of them locate the lid along the case as well as across it.
  - **Clearance:** at this length they stay clear of the USB stack (X 4.26 and up), the buttons and a ribbon plug on the
    header (top about 11.5 above the board). The two header-end tabs reach X 2.1 at the root, the edge of a 40-wire ribbon
    (X 2.1–52.9). The ribbon may touch them there, and that's accepted because the ribbon is flexible. No tab runs along
    an end wall, so the header slot stays fully open.
  - **Wedge, so the lid glides into place:**
    - **Inner face:** tapers from 2.4 mm thick at the root to 1.9 mm at the tip.
    - **Face toward the side wall:** flat and parallel to the wall (0.2 mm clearance) for the first 2 mm below the lid,
      where the snap bump sits. Below that it tapers 0.5 mm inward to the tip.
    - **End face toward the end wall:** the same flat-then-lead-in profile, so each tip narrows in both directions.
    - The tip is about 1.4 mm thick. Its vertical outer corner follows the wall's rounded inner corner, 0.2 mm clear.
  - **Snap bump:** each tab carries a bump on its side-wall face, about 0.4 mm high with 30° ramps on both faces, centred
    1 mm below the lid. It works as a detent rather than a hook: a firm lever pops it out without bending the tab much. The
    dents in the side walls sit level with the bumps, 1 mm below the lid's underside. At the header end that's above the
    ribbon-plug recess, where the wall is full thickness.
  - Bump height and angle, dent fit, tab length and drop, wedge thicknesses and the root fillet go into named constants, so
    they can be tuned after a test print.
- **LED:** the LED (board X 0.3–1.6, Y 17.4–19.1) sits under the solid frame. The frame narrows locally there, so two or
  three honeycomb cells (3.5 mm or less) sit directly over it. There's no separate window.

## 6. Assembly (goes into the docs page)

1. Tilt the board so the USB and RJ45 go into their cable-wall holes and lower the header end onto the standoffs. Push
   four M2.5 × 12 countersunk screws in from the back, through the standoffs and the board, and tighten an M2.5 nut on
   each from the top.
2. Clip the pigtail onto the antenna socket, push the SMA bulkhead out through the right wall from inside, and tighten
   its nut on the outside.
3. Optional: plug wiring onto the 40-pin header and lead it out through the header-end slot.
4. Set the lid on so the corner tabs drop into the corners, and press until the four tabs click.
5. To open: lever the lid up through the grille with a small flat screwdriver, near a tab.

## 7. Repository layout and tooling

- **Model code:** `case/` at the repository root, next to the throwaway `case/sketches/`.
  - The model is written with **build123d** (Python), in the repo style: snake_case, `ruff` lint and format.
  - Board-frame facts are named constants with a note on how each was measured. Where it's practical, they're derived
    from the STEP model at build time and checked against the constants.
- **Board model:** a script under `tools/` downloads the zip, checks its hash and extracts the STEP into `.artifacts/`
  (already ignored by git).
- **Make targets:**
  - `make case`: builds `dist/case/p4_case_base.{stl,step}` and `dist/case/p4_case_lid.{stl,step}`, plus preview PNGs.
  - `make case-test`: runs the checks in section 8.
  - Both get `## ` help comments.
  - Python dependencies run through `uv`/`uvx`, as the other tools do. Nothing is installed globally.
  - The case targets run locally only. They are not added to CI, `make test` or release packaging; that can come later.
- **Docs:** `docs/case.md`, covering print settings, screws and parts, and assembly. It is indexed in `docs/README.md`.
- **Styleguide:** `STYLEGUIDE.md` gets a short "Case model" section, covering where the code and tests live and how the
  tests are named.

## 8. Verification

These are automated checks, run against the vendor STEP:

- With the board in its final position, the board model and either printed part don't intersect.
- Clearance between them is at least 0.3 mm everywhere except the intended contacts: standoff tops, the hole bores and the screws.
- Every opening (USB-C, USB-A, RJ45, microSD, both buttons, header slot, SMA hole) has an unobstructed straight path from
  outside to the part it serves.
- **Fitting motion:**
  - The board (rounded PCB plus the boxes of the parts above it) is tilted header-end up about its bottom front edge.
  - **Drop in:** the start tilt is the smallest whole angle at which the tilted board, held back so its connector fronts
    are just inside the cable wall, drops in between the walls.
  - **Slide:** at that tilt it slides forward until the connectors are at their final depth, checked every 0.5 mm.
  - **Swing down:** it rotates to flat, checked every 1°.
  - The board never intersects the base, and the start tilt is reported. The model gives 5°.
  - If no such motion exists with the margins in 5.2, stop and report back. Don't open up the cable-wall holes without
    asking.
- A ribbon-plug block (55.9 × 9 × 9 mm, seated on the header body, centred on it) fits with at least 0.5 mm clearance to
  the base walls. With the lid on, there's at least 1.3 mm between the top of the block and the lid underside for the
  ribbon.
- Lid on: the tabs, bumps and grille don't touch the board, its parts, the SMA envelope, the ribbon-plug block or the
  ribbon path (X 2.1–52.9, from the top of the block up into the header slot).
- Each board-screw nut circle (Ø6.5 on top of the board around the mounting hole, 2.5 mm tall) is free of board parts.
- Both keyhole head volumes (the L sweep plus the head radius, 2.5 mm tall) are free of the board and its underside parts.
- From straight above the lid, the LED's top face is visible through at least one grille cell.
- No lid opening, and no gap in the header-end slot, is wider than 3.5 mm.
- With the lid fitted, the lid and the base do not overlap anywhere.
- Both parts are watertight solids.
- Printability: with the stated print orientations, there are no unsupported overhangs past 45°, except bridges of
  18 mm or less (the cable-wall openings).

The physical check is the user's: they print both parts, fit the board, plug every connector, press both buttons with a
pencil, insert and eject a card, fit and remove the lid, and hang the case both ways.

## 9. Left to the builder

- Fillet and chamfer sizes.
- The exact grille layout within the stated hole size.
- The thumb-scoop shape.
- The exact dent depth and position, within section 5.3.
- Slicer hints for the lid in `docs/case.md` (perimeters and infill around the tabs).
- The SMA hole's exact height, within the clearance rule.
- The exact clearance-hole and countersink sizes, as named constants.

## 10. Follow-ups (not now)

- Pry cut-outs or another tool-free way to open the lid.
- A printed light pipe for the LED.
- Clip-in table feet.
