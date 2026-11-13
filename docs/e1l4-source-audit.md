# Toxic Dump (E1L4): source comparison

Compared the registered E1L4.MAP in the supplied DUKE3D.GRP with the user's
`source.zip` (original `source/PREMAP.C`, `GAME.C`, `SECTOR.C`, `ACTORS.C` and
`PLAYER.C`). This is a targeted comparison of mechanics used by the level,
not a complete playthrough or proof that every effect is reproduced.

## Confirmed and addressed

- **Three-position switch combination:** The original counts DIP/TECH/ALIEN
  switches in a tag group and only activates the mechanism after the other
  switches match their hitags. The port had activated on every press and did
  not synchronise ordinary linked sprite switches. The original combination
  logic and linked-switch animation are now implemented. The underwater
  entrance's tag 185 has three switches at different heights; the regression
  stages Duke at the required swimming heights and uses the actual use button.
- **Timed doors (SE 10):** Nine SE 10 effectors in E1L4 had no implementation,
  so doors could stay open indefinitely. Restored the source timer and the
  occupied-door / animation guards. Tested the actual door in sector 272.
- **Currents (SE 24):** The original doubles SE 24's map speed at spawn and
  applies the player's current through movement velocity. The port skipped
  that initialization and moved Duke directly, bypassing movement clipping.
  Corrected speed initialization and momentum, and adjusted eligible loose
  sprite drift. E1L4 has 116 SE 24 markers; one real current is regression-tested.
- **Forcefield initialization:** Original PREMAP adds blocking and hitscan
  flags to active W_FORCEFIELD walls (and disables dark ones). Restored this
  initialization; tested an E1L4 forcefield's flags.

Existing targeted tests also exercise one water dive/surface pair, one SE 29
wave floor, the entrance-switch approach angles and monitor stability, one
SE 30 movement sequence, the entry loadout, and forced level progression.

## Known remaining gaps / risks

- **SE 36 shooters:** Two original E1L4 automatic shooters (sectors 479 and
  508, using the chaingun tile) remain unimplemented in the port.
- **SE 30 subway:** Basic locator travel is implemented, but original
  forcefield toggling and crush behavior are not. The current smoke test
  stages operation and does not establish a full end-to-end passenger ride.
- **Forcefields:** Initial blocking flags are restored, but original
  switch-controlled forcefield toggling is not fully implemented.
- **SE 33 earthquake debris:** The two E1L4 debris emitters do not generate
  the original random scrap effect.
- The map is pruned to fit the 32X memory budget. Decorations, cyclers and
  some ambience are reduced; moving machinery, combat, other switch groups,
  water links, exits and scripted destruction need broader in-game coverage.
  E1L4 has four unpaired SE 7 tags in the original map itself; their intent
  has not been established and they should not be blindly paired.
- Only spot tests and emulator boot/level checks have been run. The level is
  still experimental and a normal full playthrough remains necessary.
