# NewGRF work — tram set for AmiTTD

`poltrams.grf` here is the **AmiTTD edition** of the "PolTrams" set
(v7068M, c889cbdb00f8) by Sojita and Voyager One (graphics) and McZapkie
(NML code, graphics), released under **GPL v2**. That licence is what lets
us modify and redistribute it; the changes are listed below and the set's
own name and description in the NewGRF window say it has been modified.

The GRF ID (`4D 43 01 07`) and the file name are deliberately unchanged, so
existing savegames and `openttd.cfg` entries keep working. The checksum does
change, so OpenTTD says *"Compatible GRF(s) loaded for missing files"* once
when loading a save made with the original — that is expected and harmless.

## What was changed

1. **Every vehicle renamed to a non-Polish equivalent.** The artwork is
   generic enough to stand in for its western and Czech counterparts, so only
   the names changed — no sprite was touched. Five intro dates moved to match
   the vehicle the new name refers to (see `YEARS` in `tools/retram.py`).
2. **Freight trams carry one cargo.** They used to offer a whole cargo class;
   the cargo-class properties are cleared and an explicit cargo (goods) set,
   because this port has no refit for road vehicles and a refit-only cargo
   list is unreachable.
3. **The purchase-list blurbs no longer tell the player to use refit** to add
   trailers. They cannot: refit is off for road vehicles here and the two-car
   consist is given outright.

The two-car consist itself is NOT a GRF change — this set already returns a
trailer from its articulation callback unconditionally. It is the port's cap
in `GetNextArticPart()` (`src/articulated_vehicles.cpp`) that stops at two.

## Rebuilding

Needs `grfcodec` (6.0.6 here) and Python 3.

```sh
grfcodec -d -t -p 2 poltrams-original.grf     # -> sprites/poltrams.nfo
python3 tools/retram.py sprites/poltrams.nfo  # rewrites it in place
grfcodec -e -p 2 poltrams                     # -> poltrams.grf
```

`-p 2` is the **Windows** palette; decoding with the DOS palette (`-p 1`)
makes the artwork look corrupt.

`retram.py` regenerates only the pseudo-sprites it edits and copies every
other line verbatim. That matters: the `.nfo` carries escape tokens (`\D=`,
`\2*`, `\7!` …) for action D/7/9/2 operators, and re-emitting everything
through a plain hex parser drops them and yields a GRF that dies with
*"Read past end of pseudo-sprite"*.

## Inspection tools

- `tools/dump4.py` — vehicle names per engine id
- `tools/dump0.py` — Action 0 properties, decoded and named
- `tools/artic.py` — resolves the articulation callback statically, walking
  the Action 2 chains the way OpenTTD binds them. Answers "how many cars does
  this engine get, and does it depend on the cargo subtype?" without running
  the game.
