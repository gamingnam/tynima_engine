# {{name}}

A first-person scene built on [Tynima](https://github.com/gamingnam/tynima_engine).

```sh
tynima run     # play it; save src/game.lua while it runs and it reloads
tynima cook    # cook every model under assets/ into cooked/
tynima build   # cook, and build a native game module if this project has one
```

The mouse looks, W/A/S/D walks, Shift runs, Space jumps. The left button
shoves whatever it is pointed at, F is the torch, Tab gives the mouse back
to the desktop, R stands the crates up again, Escape quits.

`src/game.lua` is the whole game. The player is a capsule that never tips
over: the script says how fast it wants to go, the physics world decides
where that leaves it, and the camera is put where the capsule ended up. The
gun is a ray and an impulse — what it hit comes back out of the cast,
including the mark the body was made with, which is how a crate is told from
a wall.

The room is the `PILLARS`, `STEPS`, `CRATES` and `BALLS` tables near the top;
how it moves is the numbers above them.
