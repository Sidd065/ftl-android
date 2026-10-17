// Addresses and struct offsets in the game (FTL for iPad 1.6.21, arm64). ux.c checks the
// build's UUID before any of these are used.
#pragma once

// ---- functions ----
#define G_LOG_FN 0x10006051cull       // printf-style logger, compiled to `ret`
#define G_POST_EVENT 0x100117268ull   // void post_event(sil_event *)
#define G_CAPP_PTR 0x100241fc0ull     // CApp *
#define G_TIME_BASE 0x100250c88ull    // double, event timestamp base (s)
#define G_TOGGLE_PAUSE 0x100180240ull // void (CommandGui *): the pause hotkey's action
#define G_HOTKEY 0x100040728ull       // int (const std::string *action): `return 0` on iOS
// DoorBox::KeyDown: `cbz` that skips the open / close hotkey while that button is hidden
#define G_DOOR_KEY_OPEN 0x100097f9cull
#define G_DOOR_KEY_CLOSE 0x100097fe4ull
#define G_CAPP_ONRENDER 0x10000bd04ull        // void CApp::OnRender(CApp *), on the game's thread
#define G_BUTTON_ONRENDER 0x10016b164ull      // void Button::OnRender(Button *)
#define G_TEXTBUTTON_ONRENDER 0x10016c040ull  // void TextButton::OnRender(TextButton *)
#define G_SHIPMANAGER_ONRENDER 0x1000e4e98ull // void ShipManager::OnRender(ShipManager *, bool interior, bool doors)
#define G_CSURFACE_MATRIX 0x100253e10ull    // float[16]: CSurface's current matrix (column-major)
// OnRender of windows that take all input while they're open
#define G_CONFIRM_ONRENDER 0x1000f3fc8ull     // ConfirmWindow
#define G_MENUSCREEN_ONRENDER 0x10013238cull  // MenuScreen, the game menu
#define G_OPTIONS_ONRENDER 0x100080b08ull     // OptionsScreen
#define G_CHOICEBOX_ONRENDER 0x1001586f0ull   // ChoiceBox, events
#define G_LANGUAGE_ONRENDER 0x10007f2c0ull    // LanguageChooser
#define G_STARMAP_ONRENDER 0x100071308ull     // StarMap
#define G_STORE_ONRENDER 0x10010d0b4ull       // Store
#define G_TABBED_ONRENDER 0x1000f5e24ull      // TabbedWindow: upgrades, crew, cargo
#define G_INPUTBOX_ONRENDER 0x1001ac7d0ull    // InputBox
#define G_GAMEOVER_ONRENDER 0x10008e228ull    // GameOver

// ---- CApp ----
#define CAPP_GUI 0x10       // CommandGui *
#define CAPP_MENU 0x20      // MainMenu (first byte: open)
#define CAPP_STARTUP 0x40b8 // language chooser / startup screen up (game ignores keys)
// MainMenu fields: sub-screens that Escape closes
#define MENU_SUBSCREEN_OFFSETS {0x26b8, 0x3d00, 0x3fd0, 0x26c8, 0x15f0}
#define MENU_CONTINUE_ACTIVE 0x80  // MainMenu: the Continue button is active (there's a saved run)
#define MENU_ITEM 0x450            // MainMenu: int, the button just pressed; CApp acts on it (9: Continue)

// ---- CommandGui ----
#define GUI_CREW_CONTROL 0x258   // CrewControl
#define GUI_SYS_CONTROL 0x530    // SystemControl
#define GUI_FTL_BUTTON 0x1c38    // FTLButton (a GenericButton)
#define GUI_STARMAP 0x1e90  // CommandGui: StarMap * (+8 open, +0x88 int x, y, +0x900 choosing the next sector)
#define GUI_SHIP 0x1e98     // CompleteShip *, the player's
#define GUI_FOCUS_WINDOWS 0x1ea0  // CommandGui: std::vector<FocusWindow *> (FocusWindow +8: bool open)
#define GUI_UPGRADES 0x3cf0 // CommandGui: the ship / crew / cargo window (+0x68 tab)
#define GUI_PAUSED 0x3c58   // CommandGui: bool paused
// CommandGui: boxes that take keys before any focus window (the game menu, options,
// confirmations); each has its open flag at +8. While one is up it covers everything.
#define GUI_MODAL_OFFSETS {0x5948, 0x2248, 0x27e8, 0x3c68, 0x5648, 0x28f8}
#define GUI_CHOICE 0x1f68   // CommandGui: ChoiceBox, the event window (+8 open)
#define CHOICE_FRAME 0x28   // ChoiceBox: WindowFrame * (int x, y, w, h from the box's corner)
#define CHOICE_LIST 0x48    // ChoiceBox: std::vector<ChoiceText>, 584-byte entries
#define CHOICE_COLUMN 0x60  // ChoiceBox: unsigned; more choices than this use a different, wide layout
#define CHOICE_FONT 0x88    // ChoiceBox: int font size, 10..13 (the desktop font +/- hotkeys)
#define CHOICE_CENTRED 0x8c // ChoiceBox: bool; false moves the box left, clear of an enemy ship
#define CHOICE_RECTS 0x68   // ChoiceBox: std::vector of int x, y, w, h, one per choice (HUD units)

// ---- the HUD ----
#define CREWCTL_DOOR_MODE 0xb0   // CrewControl: bool, the door button is on
#define CREWCTL_BOXES 0x60       // CrewControl: std::vector<CrewBox *> (CrewBox +0: Rect box)
#define SYSCTL_BOXES 0x10        // SystemControl: std::vector<SystemBox *>
#define SYSCTL_POSITION 0x3c     // SystemControl: Point, where its boxes are drawn from
#define SYSBOX_HITBOX 0x1ec      // SystemBox: Rect, from SYSCTL_POSITION
#define BUTTON_HITBOX 0x10       // GenericButton: Rect (HUD units)
#define BUTTON_ACTIVE 0x48       // GenericButton: bool
#define GUI_WEAPON_CONTROL 0x620 // CommandGui: WeaponControl, an ArmamentControl
#define GUI_DRONE_CONTROL 0xab0  // CommandGui: DroneControl, an ArmamentControl
#define ARMCTL_BOXES 0x20        // ArmamentControl: std::vector<ArmamentBox *>, from the bottom up
#define ARMCTL_LOCATION 0x38     // ArmamentControl: Point, the open panel's bottom left
#define ARMCTL_PANEL 0x40        // ArmamentControl: Rect from LOCATION, the open panel
#define ARMCTL_OPEN 0x9c         // ArmamentControl: bool, the panel is open (else a column of small boxes)
#define ARMBOX_ICON 0xb0         // ArmamentBox: Point, where its small box's icon was last drawn (never: INT_MAX)
#define ARMBOX_LOCATION 0xb8     // ArmamentBox: Point from ARMCTL_LOCATION, its box in the open panel

// ---- ships ----
#define COMPLETE_SHIP_MANAGER 0x10  // CompleteShip: ShipManager *
#define SM_ID 0x8                   // ShipManager: int, 0 the player's, 1 the enemy
#define SM_SYSTEMS 0x28             // ShipManager: std::vector<ShipSystem *>
#define SM_WEAPONS 0x88             // ShipManager: WeaponSystem *
#define SM_CREW 0xc0                // ShipManager: std::vector<CrewMember *>
#define SM_SHIP 0x118               // ShipManager: Ship
#define SM_JUMPING 0x6f4            // ShipManager: bool
#define SHIP_ROOMS 0x10             // Ship: std::vector<Room *>
#define SHIP_DOORS 0x28             // Ship: std::vector<Door *>
#define SHIP_HULL 0x70              // Ship: int hull, max hull
#define ROOM_RECT 0x1c              // Room: Rect, ship coordinates (35 per tile)
#define DOOR_X 0x2b0                // Door: int x, y (its middle), bool vertical
#define DOOR_Y 0x2b4
#define DOOR_VERTICAL 0x2b8
#define CREW_X 0xc                  // CrewMember: float x, y (ship coordinates)
#define CREW_Y 0x10
#define CREW_SHIP 0x1ac             // CrewMember: int, the ship they're on (0: the player's)
#define CREW_DEAD 0x260             // CrewMember: bool
#define SYSTEM_HEALTH 0x224         // ShipSystem: int health, max health
#define WEAPONS_LIST 0x310          // WeaponSystem: std::vector<ProjectileFactory *>
#define WEAPON_QUEUED 0x718         // ProjectileFactory: std::vector<Projectile *>, shots fired, not yet out

// ---- the beacon map ----
#define MAP_LOCATIONS 0x28          // StarMap: std::vector<Location *> (Location +0: float x, y)
#define MAP_POSITION 0x88           // StarMap: Point
#define MAP_TRANSLATION 0xa0        // StarMap: float x, y
#define MAP_NEW_SECTOR 0x900        // StarMap: bool, choosing the next sector
