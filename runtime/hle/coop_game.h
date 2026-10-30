/*
 * coop_game.h -- a map of the game, as far as co-op needs it.
 *
 * Ace Combat 3 runs from the original executable (Rev 1 ACE3.BIN), so it has
 * no names: the co-op code reaches it through addresses. This file gives
 * every address and every field offset the co-op code uses a name and says
 * what it is. How each was found: docs/coop/memory_map.md and S2_guest.md.
 *
 *   1. game functions          (called, or intercepted when the game calls them)
 *   2. global data             (fixed addresses and pointers off the game's gp)
 *   3. data structures         (field offsets, and small accessors)
 *
 * The game keeps its globals around the gp register (GAME_GP); "gp+0x2E8" in
 * the notes is GAME_GP + 0x2E8. "The player" is always slot 0 of the slot
 * table; co-op puts the guest in slot 1.
 */
#ifndef AC3_COOP_GAME_H
#define AC3_COOP_GAME_H
#include "ac3_runtime.h"

/* ==== 1. game functions ======================================================= */

/* the flight frame and its tick */
#define FN_PLAYER_UPDATE     0x800729DCu   /* the player's aircraft, once per frame (first call of the tick) */
#define FN_INPUT_MAP         0x80028C24u   /* (controller, map, map+0x3C): controller -> the player's actions */
#define FN_TARGETING         0x8003A23Cu   /* (&args): lock-on / target selection, after the world update */
#define FN_DRAW_LAST         0x80033814u   /* the flight frame's last drawing call */
#define RA_DRAW_LAST         0x80033DA8u   /*   ... its return address */
#define RA_AFTER_TICK        0x80033A8Cu   /* the flight frame, just after it called the tick (by pointer) */

/* enemies */
#define FN_ACTIVATE_AIRCRAFT 0x80038100u   /* sets "active" on aircraft (slots 2..31) near the player */
#define FN_ACTIVATE_GROUND   0x80051498u   /* the same for ground and ship objects (slots 0x20..0x3F) */
#define FN_AI_FIRST          0x80046600u   /* the AI's first pass; 0x8003809C clears the lock-on alert before it */
#define FN_AI_PRE            0x80046854u   /* aircraft AI loops, slots 2..31, in this order: */
#define FN_AI_DECIDE         0x800469ECu   /*   decisions (target choice 0x80046124) */
#define FN_AI_MOVE           0x80046DFCu   /*   manoeuvres */
#define FN_AI_WEAPON         0x8004701Cu   /*   attacks */
#define FN_GROUND_WEAPON     0x80052FA8u   /* (i): ground/ship weapon i of WEAPON_LIST, once per frame */
#define FN_LOCK_ALERT        0x800535D4u   /* (i): weapon i locking on; raises the lock-on alert half-way */
#define FN_MISSILE_LAUNCH    0x800685C0u   /* (shooter, ..., target handle in a3): a missile */

/* HUD */
#define FN_SHOW_WARNING      0x8004287Cu   /* (n): put warning n on screen, if more urgent than the current one */
#define FN_PICK_WARNING      0x80042544u   /* the HUD's warning picker (calls FN_SHOW_WARNING) */
#define FN_PROJECT_TARGETS   0x8003B34Cu   /* targets (slots 2..63) onto the screen; leaves the GTE camera matrix set */
#define FN_DRAW_LABELS       0x8003F520u   /* target boxes and labels */
#define FN_DRAW_LABEL        0x80040740u   /* (kind, x, y): a label under a target, LABEL_* */
#define FN_BOX_SIZE          0x8003B758u   /* (depth): a target box's size */
#define FN_DRAW_RADAR        0x8003DE5Cu   /* draws the radar's blips (slots 2..63) */
#define FN_DRAW_RADAR_BLIP   0x8003DFF8u   /* (&blip): one blip */
#define FN_ROTTRANSPERS      0x800A7358u   /* Psy-Q RotTransPers(&svector, &sxy, &p, &flag) */

/* the mission */
#define FN_MISSION_RULES     0x800540E0u   /* per-mission checks on the player, once per frame */
#define FN_HAZARD_DAMAGE     0x80076FA4u   /* takes a point of health off slot 0, shakes the screen */
#define FN_FX_WAKE           0x80082E80u   /* effects around the player, all called from 0x800850D8 */
#define RA_FX_WAKE           0x8008511Cu   /*   (recognised by their return addresses) */
#define FN_FX_WEATHER        0x80084F6Cu
#define RA_FX_WEATHER        0x80085154u
#define FN_FX_SCENE2A        0x80084E6Cu
#define RA_FX_SCENE2A        0x80085198u
#define FN_FX_FLASH          0x800831D0u
#define RA_FX_FLASH          0x80090200u   /*   (called by 0x80090148) */

/* ==== 2. global data ========================================================== */

#define GAME_GP              0x800BCB74u
#define SLOT_TABLE           (GAME_GP + 0x2E8u)  /* -> N_SLOTS entity pointers; slot 0 = the player */
#define N_SLOTS              64u
#define SLOT_FIRST_AIRCRAFT  2u        /* aircraft: slots 2..31 */
#define SLOT_FIRST_GROUND    0x20u     /* ground and ship objects: slots 0x20..0x3F (WEAPON_LIST) */
#define WORLD_PTR            (GAME_GP + 0x2E0u)  /* -> the world (WORLD_*) */
#define SCENE_PTR            (GAME_GP + 0x2ECu)  /* -> the scene (SCENE_*) */
#define WEAPON_LIST          (GAME_GP + 0x2D8u)  /* -> 32 ground/ship weapon entities (slot 0x20 + i) */
#define MISSION_STATE        (GAME_GP + 0x340u)  /* -> the mission's flags (word 0) */
#define AI_RECS              (GAME_GP + 0x34Cu)  /* -> the AI's records, AIREC_SIZE per slot */
#define HUD_LOCK             (GAME_GP + 0x354u)  /* 8 bytes: the lock-on in progress (state, slot, animation) */
#define RADAR_VIEW           (GAME_GP + 0x43Cu)  /* -> the radar's centre and rotation (in the scratchpad) */
#define LOW_FRAMES           (GAME_GP + 0x44Cu)  /* byte: frames too low; PULL UP after 30 */
#define RETICLE_RING_INDEX   (GAME_GP + 0x4FCu)  /* gun reticle: ring index, reticle entry */
#define MISSILES_PTR         (GAME_GP + 0x50Cu)  /* -> the missile manager (MSL_*) */
#define BLOCK_PTR            (GAME_GP + 0x514u)  /* -> the player's aircraft state block (BLOCK_*); moves between missions */
#define PLAYER_MODE_PTR      (GAME_GP + 0x520u)  /* -> the player's mode (MODE_*), inside the block */
#define CAMERA_STATE_PTR     (GAME_GP + 0x524u)  /* -> the player's camera/target state (CAMST_*), inside the block */

#define SCENE                0x800BF2D0u   /* the scene SCENE_PTR points to during flights */
#define ENGINE_SOUND         0x800C5830u   /* the player's engine sound channels, 0x40 bytes */
#define LIBRARY_WORD         0x800BD0C0u   /* written by the player update */
#define SHOT_DOWN_EFFECT     0x800BCEA0u   /* screen effect after the player is shot down (0x800301A8) */
#define CTRL_PORT2           0x800BF388u   /* the game's decoded record for controller port 2 */
#define INPUT_MAP            0x800BF3F0u   /* the player's actions, 0x40 bytes (+0x3C: 4 more) */
#define POSITION_COPY        0x800C7BE8u   /* written by 0x80072114 every frame */
#define TARGETING_STATE      0x800C6D60u   /* 0x18 bytes; +0x08..0x12 decided by the HUD while it draws */
#define ALERT_STATE          0x800C6EC0u   /* 0x10 bytes; +ALERT_BITS: ALERT_* bits */
#define ALERT_BITS           0x0Cu
#define ALERT_LOCKED_ON      0x01u         /*   an enemy weapon is locking on */
#define ALERT_MISSILE_CLOSE  0x02u         /*   a missile is close */
#define HUD_RECS             0x800C67A0u   /* -> each slot's target record (HREC_*), 4 bytes per slot */
#define HUD_RECS_C           0x800C6ED0u   /* -> each slot's record + 0x0C */
#define HUD_ARRAYS           0x800C68E0u   /* per-target arrays the HUD keeps between frames, incl. RADAR_BLIPS */
#define HUD_MISC             0x800C6EA0u   /* 0x14 bytes the HUD keeps between frames (0x800309A8) */
#define RADAR_BLIPS          0x800C6A60u   /* BLIP_SIZE per slot */
#define RADAR_MODE           0x800C6D6Eu   /* byte */
#define RADAR_RANGE          0x800C6D7Cu   /* word */
#define AI_TABLE             0x800C6D80u   /* 4 bytes per slot (AIT_*) */
#define WARN_STATE           0x800C7180u   /* the warning on screen: number, time left, two animation records */
#define RETICLE_RING         0x800C79C8u   /* gun reticle: 32 bullet positions, one per frame */

/* ==== 3. data structures ======================================================== */

/* entity (one per slot; the players' are ENT_SIZE bytes) */
#define ENT_SIZE         0x58u
#define ENT_WORD         0x00u   /* handle (low 6 bits: slot) and ENT_* flags */
#define   ENT_LIVE       0x10000u  /* the slot is in use */
#define   ENT_ACTIVE     0x8000u   /* the world runs it this frame */
#define   ENT_EXISTS     0x4000u
#define ENT_POS          0x08u   /* x, y, z: 3 words, 20.12 fixed point; y grows downwards */
#define ENT_X            0x08u
#define ENT_Y            0x0Cu   /* height: larger = lower */
#define ENT_Z            0x10u
#define ENT_ROT          0x18u   /* rotation, 3 halfwords */
#define ENT_HEADING      0x1Au
#define ENT_VEL          0x38u   /* velocity */
#define ENT_HEALTH       0x4Eu   /* halfword */
#define WPN_TARGET       0x5Bu   /* ground/ship weapons: target slot, 0xFF = none */

static inline int ent_live(u32 e)      { return (MEM_R32(e + ENT_WORD) & ENT_LIVE) != 0; }
static inline s32 ent_coord(u32 e, int c) { return (s32)MEM_R32(e + ENT_POS + 4u * (u32)c); }   /* 0 x, 1 y, 2 z */
static inline s32 ent_x(u32 e)         { return (s32)MEM_R32(e + ENT_X); }
static inline s32 ent_y(u32 e)         { return (s32)MEM_R32(e + ENT_Y); }
static inline s32 ent_z(u32 e)         { return (s32)MEM_R32(e + ENT_Z); }
static inline s32 ent_health(u32 e)    { return (s32)MEM_R16S(e + ENT_HEALTH); }

/* the world */
#define WORLD_FLAGS      0x000u  /* bit 0 hides the HUD (the shot-down camera) */
#define WORLD_STATE      0x004u  /* 4 = flying */
#define WORLD_FRAME      0x00Cu  /* frame counter */
#define WORLD_OT         0x010u  /* the ordering table being drawn: one of: */
#define WORLD_OT0        0x018u
#define WORLD_OT1        0x01Cu
#define WORLD_PACKETS0   0x020u  /*   and their packet buffers */
#define WORLD_PACKETS1   0x024u
#define WORLD_CAMERA     0x028u  /* -> the camera object (CAMOBJ_*): WORLD_CAMERA_OBJ */
#define WORLD_VIEW_SETUP 0x02Cu  /* 0x34 bytes, written by the view setup (0x800332E4) */
#define WORLD_CAMERA_OBJ 0x0B8u  /* the camera object, 0xE8 bytes */
#define WORLD_SHAKE      0x28Cu  /* screen shake, 8 bytes */
#define CAMOBJ_POS       0x04u   /* camera position, 3 words */

/* the scene */
#define SCENE_FLAGS      0x000u  /* bit 0x20000000: the flight is paused/over */
#define SCENE_MISSION    0x38Cu  /* halfword: mission scene number */
#define SCENE_TWO_PLACES 0x394u  /* nonzero: each player can have two attackers */
#define SCENE_SHOT_DOWN  0x958u  /* times the player was shot down */

/* the player's mode */
#define MODE_STATE       0x04u   /* 1 = has control, 2 = mission intro */

/* the player's aircraft state block */
#define BLOCK_SIZE       0xE0u
#define BLOCK_MISSILES   0xB4u   /* halfword: missiles left */

/* the player's camera/target state */
#define CAMST_TARGET     0x18u   /* the locked target (the launch code reads it) */

/* the AI's records and table */
#define AIREC_SIZE       0xB8u
#define AIREC_LAST_TARGET 0x3Fu  /* byte: target slot */
#define AIREC_TARGET     0x60u   /* byte: target slot */
#define AIT_TARGET       1u      /* AI_TABLE + 4 * slot: byte, its target slot */
#define AIT_ATTACKER1    2u      /*   bytes: the slots attacking it, 0xFF = free place */
#define AIT_ATTACKER2    3u      /*   (the second only when SCENE_TWO_PLACES) */
#define NO_SLOT          0xFFu

/* a target's HUD record */
#define HREC_SIZE        0x34u
#define HREC_FLAGS       0x0Cu   /* HREC_NEAR: near the player (distance under about 34,000) */
#define   HREC_NEAR      0x80u
#define HREC_DIST        0x10u   /* distance to the player (for far targets refreshed every 16 frames) */

/* a radar blip */
#define BLIP_SIZE        12u
#define BLIP_FLAGS       0u      /* bit 0 shown, bit 3 long range, bit 2 radar mode 2 */
#define BLIP_KIND        2u      /* BLIP_* colour */
#define   BLIP_ENEMY     1u      /*   red */
#define   BLIP_ALLY      2u      /*   blue */
#define   BLIP_YELLOW    3u
#define BLIP_HEADING     4u
#define BLIP_LEVEL       5u      /* 0 below, 1 level, 2 above */
#define BLIP_X           6u
#define BLIP_Y           7u
#define BLIP_DIST        8u

/* target labels (FN_DRAW_LABEL) */
#define LABEL_TGT        1u
#define LABEL_NTGT       2u
#define LABEL_FRND       3u

/* the missile manager: per target slot, who has missiles coming */
#define MSL_TARGETED     0x00u   /* word: bit n = slot n has a missile coming (slots 0..31) */
#define MSL_LATEST       0x08u   /* byte per slot: the latest missile at it */
#define MSL_FIRED_BY     0x48u   /* byte per slot: missiles it fired */
#define MSL_HOMING       0x88u   /* byte per slot: missiles homing on it */

#endif
