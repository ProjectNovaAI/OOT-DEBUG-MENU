# PastyPixel's Bomb T-Pose

Game version 1.1.1, both ages. Found and checked on 2026-11-13 with the debug console's frame recorder (`snap`), scripted
controller (`pad`) and clips. The debug menu's "PastyPixel's Bomb T-Pose" toggle reproduces it without the ocarina
(below).

## What it looks like

Link pulls a bomb out while moving. Instead of lifting it straight over his head, he first holds both arms out to
the sides at shoulder height, the bomb in his right hand. About 16 to 20 frames later the arms go up and the carry looks
normal. Every bomb pulled out while moving does this until something clears it.

## How to get it in the game, every time

1. Take the ocarina out (D-pad left).
2. Hold A. Keep holding it.
3. 6.25 seconds after A went down, Link runs out of breath and inhales. Press B while he inhales: the ocarina goes
   away. The window lasts about 1 second.
4. Walk or run, and keep moving until Link's face has changed to its walking look (about half a second of real
   movement). Then pull a bomb out (ZR): it comes out sideways.
5. Put it away or throw it, keep moving, and pull another. It comes out sideways again.

The time in step 3 is measured: in two runs with A pressed 3.1 s and 5.6 s after the start, the inhale began 6.26 s
and 6.25 s after A, and lasted 1.0 s.

### What clears it

- Standing still for about 10 seconds. Link's idle animation gives the face its own mask back.
- Pulling a bomb out while standing still, or right as you start to move. The standing pull (WeaponEquipOn) gives the
  face its normal mask back. Walking into a wall counts as standing.
- Letting the inhale finish with the ocarina still out. The game puts the normal mask back at the end of the inhale,
  which is why B has to land during it.

### What keeps it

Walking or running (32 s of walking in four directions kept it), and any number of pulls, put-aways and throws while
moving (three in a row kept it).

## The exact reason

Link's animation is a stack of layers, each with a bone mask that says which bones it moves. On the player
(`?AS/Player_Adult.root.asb` or `?AS/Player_Child.root.asb`) the stack is: 0 legs (Wait, Move, Brake, ...), 1 upper body
(CommonUpper, WeaponEquipOn, the bomb carry), 2 and 3 the hands, 4 the face, 5 the fingers, 6 the head turning, 7 and 8
others. The face layer sits above the upper body, so whatever it does to a bone wins over the carry.

A layer's mask is built from a short definition: a list of bones, each with a flag that sets or clears that bone and
everything under it. The face layer's own definition lists Root and Face_Root (bone 107 at both ages), so it moves the
face and nothing else.

1. **The inhale widens the face mask.** When a held note runs out of breath, the face layer plays DemoOcarinaBreath.
   That state gives the face layer's mask a 14-bone definition: Root, Spine_2 (the chest, both arms and the head), the
   two Weapon bones and the ten finger bones. The bones are 0, 11, 23, 68, 20, 24, 29, 34, 39, 65, 69, 74, 79 and 84 on
   the child, and 0, 6, 18, 66, 14, 19, 24, 29, 34, 62, 67, 72, 77 and 82 on the adult. The face layer now moves the
   chest and both arms, so the breathing shows in the upper body.
2. **Putting the ocarina away mid-inhale skips the reset.** When the inhale ends on its own, the game gives the face
   layer its normal definition back (seen at the end of every inhale). Pressing B during the inhale ends the ocarina
   before that happens. The face states that come next, FaceDefault and FaceLocomotion, name no mask of their own,
   so the wide one stays.
3. **A pull while moving leaves the face layer with nothing to play.** The face layer takes the body's node name
   when it has no face animation for it. When Link pulls a bomb while FaceLocomotion is playing, the face layer moves to
   the Move node, which has no face animation: no state in either slot, active slot 0xff. It fades into that empty
   state over about 14 frames and stays there until FaceDefault starts, 16 to 20 frames after the pull.
4. **The empty face layer poses the arms.** While the face layer is on Move with no state and the wide mask, the bones
   under that mask get the face layer's pose instead of the carry's: both arms out, the right hand at shoulder height.
5. **FaceDefault fades the arms back up.** When FaceDefault starts, the face layer has an animation again. It has no
   arm keys, so as it fades in, the arms return to the upper body's carry pose over the head. The wide mask is still
   there for the next pull.

### How this was checked

- Steps 1 and 2: the face mask definition was read from memory on every change (bone count 14 against 2). It became
  the 14-bone one when DemoOcarinaBreath started, went back to 2 bones 1.0 s later at the end of the inhale with the
  ocarina out, and stayed 14 bones when B was pressed during the inhale.
- Step 3: recordings of every frame show the same gap after every pull while moving: face node Move, both slots
  0xffff, active slot 0xff. This happens with the bug or without it.
- The mask alone decides it. The toggle changes nothing but the face layer's mask definition, and only while a
  bomb is lifted or carried. With it off, the gap still happens and the arms go straight up (arm bones about 1.0 from
  the bug pose). With it on, the gap shows the bug pose, the same as the ocarina bug: within 0.01 to 0.05 of a natural
  recording on the child, and within 0.12 between two adult runs. Distances are the largest difference of any
  rotation matrix entry over the clavicle, upper arm, forearm and wrist bones of both arms.
- Step 4's pose is close to the same every time, but not exactly. Across separate sessions, walking speeds and
  directions it agreed within 0.01 to 0.12 in most runs; one run walking the other way was up to 0.38 off in the upper
  arms. It does not occur anywhere in normal play (the nearest frame in normal pulls, carries and walking is 0.8 away).
  It is not the model's bind pose (0.9 away), and it is not identity rotations. Despite the name, it is not a true
  T-pose.
- A standing pull also leaves the face layer with no state (on the WeaponEquipOn node), and the toggle gives it the
  wide mask too, yet the arms go straight up, exactly as in a normal carry. The side pose comes only from the Move node.

### Not worked out

How the face layer computes the side pose's values on the Move node with no state. Its layer update (main+0x7310d0)
has not been followed through for that case. The pose is not a frame of any animation recorded in normal play.

## The toggle

"PastyPixel's Bomb T-Pose", under No void-out in the debug menu. Hotkey `bombbug`, console `bombbug [0|1]`, kept as
`debug_bomb_bug`.

- `recomp/patches-v111.txt` (set dbgframe): the animation owner's update (main+0x29d740) updates every layer, then
  loads the number of layers again at main+0x29e548 to blend them. That load calls page+0xec0 (`debug_game.c`
  `debug_bomb_layers`), which runs `debug_move.c` `dbg_bomb_layers` and then makes the load.
- `dbg_bomb_layers`: while the option is on and the player lifts or carries a bomb (PlayerLifter state 2 or 3 and the
  blackboard's LiftableType 4), it checks the face layer's mask definition. If it is not the breathing one, it keeps the
  game's definition and writes the 14-bone breathing definition for the player's age into the same arrays. Then it bumps
  the definition's version and builds the mask bits through the definition's own method (vtable +0x30), as the game
  does when a state sets a mask. When the carry ends or the option goes off, the kept definition goes back the same
  way, unless the game has set another one since.
- The face states play as usual. The result is the game's own bug on every pull while moving, with no ocarina and
  nothing that clears it, from the first frame of the carry, and nothing different on a standing pull.
- Tested 2026-11-13, adult and child: pulls while walking come out sideways and then go over the head. Standing pulls
  match a normal carry. Switching it off puts the face definition back (bits and bone count checked).

## Memory layout used (1.1.1)

- Animation owner: AS component +0x70. Blackboard +0x58, its ints at +0x18 (count, then the values from +8; LiftableType
  is int 14: 4 bomb, 8 nothing). Layers at +0x198, 0x48 apart, method table main+0x4c29788.
- Layer: +0x10 node name, +0x50 mask object, +0xbc the two slots' states (u16, 0xffff none), +0x190 four fade floats
  (1, 1, 0, 1 when a change has finished fading in), +0x250 the slots' state names, +0x3c5 active slot (0xff none).
- Mask object: +0x8 the bit set, +0x10 the bits, +0x18 the definition, +0x30 flags (bit 0: changed, bit 4: just built).
- Definition: +0x20 room for flags, +0x28 the flag bytes, +0x30 version (u32), +0x34 bone count (u16), +0x38 room for
  bones, +0x40 the bones (s16 skeleton number, s16 bone). Flags 0x83 set the bone and its subtree, 0x80 clear it. Its
  method at vtable +0x30 builds the bits.
- PlayerLifter component: +0x2b0 state (0 nothing, 2 lifting, 3 carrying).
