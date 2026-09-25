# {{name}}

A 2D platformer built on [Tynima](https://github.com/gamingnam/tynima_engine).

```sh
tynima run     # play it; save src/game.lua while it runs and it reloads
tynima cook    # cook every model under assets/ into cooked/
tynima build   # cook, and build a native game module if this project has one
```

A and D (or the arrows) run, Space jumps — let go of it early for a short
hop. Collect the coins and reach the flag. R starts the level again, Escape
quits.

`src/game.lua` is the whole game. The level is the `BLOCKS`, `COINS` and
`CRATES` tables near the top of it; how it plays is the dozen numbers above
them. Change one while the game is running and it is under your hands a
quarter of a second later.

Everything the game makes sits on the z = 0 plane, and the camera looks
straight down -z at it: things at one depth are all scaled alike, so a 3D
engine draws a flat game. The scenery behind is not at that depth, which is
why it drifts as the camera follows you — parallax, for nothing.
