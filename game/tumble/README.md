# Tumble

The third game on Candela, and the one that exercises the engine's new
**physics** pillar (Jolt): a capsule character you drive around a walled arena
to shove, stack, and topple a heap of dynamic boxes.

Where Lightkeeper showed the renderer could ship gameplay and Emberwake added
combat and AI, Tumble is deliberately physics-forward — the point is real 3D
rigid-body simulation with a driveable character controller, the thing the
engine could not do before the gameplay-systems work.

## Status — increment 1: headless physics core

The simulation is in and proven headlessly, no GPU required:

```
candela-tumble --selftest
```

builds the sandbox (floor + four walls + a pyramid stack of dynamic boxes + a
capsule character), charges the character straight into the stack, and asserts
that the character stays grounded through the shove while the stack scatters —
in the same pure-simulation style as `lightkeeper-leveltest` and the engine's
`--physicstest`.

This also drove the first real engine addition the game motivated: a
**character-movement API**. Jolt's `JPH::Character` was created but had no way
to be driven, so `CharacterController` gained a runtime `desiredVelocity`
(horizontal drive; gravity owns the vertical axis) and an `onGround` readback,
which `PhysicsSystem` applies each fixed step. Pushing dynamic boxes then comes
for free — the character is a real rigid body.

## Next increments

- **Render + play:** draw the arena and boxes (the character is the downloaded
  rigged Fox), a third-person follow camera, camera-relative WASD driving
  `desiredVelocity`, verified with `--screenshot`.
- **Feel:** Fox idle/run animation via the `Animator`, footstep and box-impact
  spatial audio.
