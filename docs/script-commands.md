# UnrealScript console commands (exec functions)

Extracted from the cooked scripts (`APGame.u`, `Engine.u`, `GameFramework.u`, `APGameContent.u`) with
`tools/re/sigs.py` (functions with FUNC_Exec 0x200). Parameters in call order. State-specific overrides
and the `APInput_*` key handlers are left out. CheatManager commands need `EnableCheats` first.

Bytecode sizes of 2-3 bytes are empty stubs in the release build: GiveAllAchievements, PrintMaterials,
RunPathNodeTest, ClearSoundModeStack, Loadout, AllAmmo, AllWeapons, EndPath, KillBots.


## APDebugCameraController (APGame.u)

- `ToggleDebugHUD`

## APGameInfo (APGame.u)

- `ShowAIDebugInfo`
- `Benchmark` Float framespeed
- `AIDBGSpotted`
- `AIDbgPerception`
- `ShowVisionChecks` Str visionString
- `ShowAIAimError`

## APHUD (APGame.u)

- `ToggleSafetyRegion`
- `ToggleAIDist`

## APPawn (APGame.u)

- `ToggleDebugAimAttractors`
- `ShowMeleeDebug`

## APPawn_Player (APGame.u)

- `PrintMaterials`
- `AnimDebugOverlay` Name AnimNodeName
- `ToggleAILoot`
- `PauseGame`
- `UnpauseGame`
- `PauseMusic`
- `UnpauseMusic`
- `UnlockVeteran`
- `UnlockLevel15`
- `Rambo`
- `DebugGiveAmmo`
- `DrawGrenadeRange`
- `DebugAcquireHalbechData` Int Data
- `UnlockDossierInfo` Byte ContactID
- `UnlockDossierSecretFact` Byte ContactID
- `SetDossierStatus` Byte ContactID Int nStatus
- `GiveWeapon` Str weaponString
- `StripWeapons`
- `StripWeapon`
- `GiveAllWeapons`
- `GiveAllWeaponMods`
- `GiveAllArmor`
- `GiveAllArmorMods`
- `GiveAllGadgets`
- `EquipGadgets1` Int tier
- `EquipGadgets2` Int tier
- `SetStoryVar` Str varToSet Int NewValue
- `PrintStoryVars`
- `AdjustReputation` Byte SC Int Rep
- `PrintReputation`
- `SetMissionStatus` Byte currMissionID Byte NewStatus
- `PrintMissionStatus`
- `SetNumMissionsCompleted` Int newCount
- `ToggleDamageTicks`
- `ToggleDisplayMovementSpeed`
- `ToggleHUDFade`
- `ToggleCamPos`
- `ToggleCopyright`
- `ToggleTempHUD`
- `GiveXP` Int NewXP
- `GiveAP` Int NewAP
- `GiveMoney` Int NewCash
- `DebugSetSpecialization` Name NewSpecialization
- `Demigod` Int SkillSet
- `DebugPurchaseSkill` Byte NewSkill
- `DebugPurchasePerk` Byte NewPerk
- `DebugPurchaseAllPerks`
- `DebugUnpurchaseSkill` Byte OldSkill
- `GiveAllAchievements`
- `PrintSkills`
- `PrintPerks`
- `ToggleMissionCheats`
- `ToggleIntelCheats`
- `DebriefCheats`
- `TrophyCheats`
- `VendorCheats`
- `EmailCheats`
- `DossierCheats`
- `ToggleShowAIStates`
- `ToggleShowAIVerbose`
- `AIDbg`
- `AIDbgFont` Str FontSize
- `AIDbgMelee`
- `AnimDbg`
- `ToggleCamera`
- `TogglePuckMode`
- `RunPathNodeTest`
- `ShowSockets` Name SpecificSocket
- `ToggleCameraBob`
- `EnemyPhysAnims`
- `RagdollRecoverEnemies`
- `RagdollRecoverPlayer`
- `LogBreak` Str Note
- `SetDeathCamera` Int cam
- `ReleasePlayerFromCSA`
- `SpecialSloMo` Float Rate
- `ToggleAimDebug`
- `ToggleHitDebug`
- `ToggleStickyDebug`
- `ToggleSoftLock`
- `ToggleWeaponEffects`
- `ToggleNewerCamera`
- `ToggleInfiniteAmmo`
- `ToggleDmgCurveDebug`
- `ToggleRecoil`
- `ToggleStickyLook`
- `ToggleSuperMike`
- `ToggleAimAssist`
- `showspread`
- `ToggleWeaponStartPosition`

## APPawn_Player_Normal (APGame.u)

- `CurrentAnimSets`
- `MeasureMPH`
- `rubber`
- `puppet`
- `jock`
- `chimp`
- `stroke`

## APPlayerController (APGame.u)

- `GadgetEnd`
- `ShowRawControllerData`
- `ShowCoverScores`
- `ImmuneToDamage`
- `ShowCombatRatings` Name TgtPawnName
- `DbgCoverMethod`
- `ToggleCQCCamera`
- `ToggleAimDOF`
- `ToggleReticuleTest`
- `ToggleMotionBlur`
- `PushSoundMode` Byte NewMode
- `PopSoundMode` Byte ModeToPop
- `ClearSoundModeStack`
- `PlayMovie` Name MovieName
- `Loadout`
- `AIDbgLock`
- `AIDbgPawn` Name TgtPawnName
- `Invisible`
- `AIDbgBlindPawn` Name TgtPawnName
- `AIDbgDeafenPawn` Name TgtPawnName
- `AIDbgDump`
- `AIDbgDumpVerbose`
- `AIDbgDumpPawn` Name TgtPawnName
- `AIDbgDumpSquad` Name TgtSquadName
- `AISetMaxAimValidations` Int newMax
- `AISetMaxPriorityRayCasts` Int newMax
- `AIInvincible` Name TgtPawnName
- `AIPause` Name TgtPawnName
- `ToggleCameraSpeed`
- `InvertLook`
- `InvertTurn`
- `ShowLPDeadZone`
- `ToggleDebugLightShow` Name LightName
- `CQCMeleeGunButt`
- `CQCMeleeKick`
- `ToggleDebugCover`
- `DisplayInputs`
- `Kill` Str Type
- `ToggleAPMovieCamera`
- `SetGamma`
- `GetAnimTreeLeafAnimations`
- `WinMiniGame`
- `PerformChainShot`
- `PerformBulletStorm`
- `ToggleSpecial`
- `StartNewElectronicMiniGame` Byte BaseDifficulty Bool bAllowBash Bool bCapDifficulty
- `StartNewLockPickMiniGame` Byte BaseDifficulty Bool bAllowBash Bool bCapDifficulty
- `StartNewHackingMiniGame` Byte BaseDifficulty Bool bCapDifficulty

## Controller (Engine.u)

- `SwitchToBestWeapon` Bool bForceNewWeapon

## PlayerController (Engine.u)

- `ToggleSubtitles`
- `ToggleTimeCode`
- `TogglePlayerStates`
- `EnableCheats`
- `FOV` Float F
- `Mutate` Str MutateString
- `Say` Str msg
- `TeamSay` Str msg
- `Camera` Name NewMode
- `Speech` Name Type Int Index Str Callsign
- `RestartLevel`
- `LocalTravel` Str URL
- `QuickSave`
- `QuickLoad`
- `DebugPause`
- `Pause`
- `ShowMenu`
- `UTrace`
- `ThrowWeapon`
- `PrevWeapon`
- `NextWeapon`
- `StartFire` Byte FireModeNum
- `StopFire` Byte FireModeNum
- `StartAltFire` Byte FireModeNum
- `StopAltFire` Byte FireModeNum
- `Use`
- `Suicide`
- `SetName` Str S
- `SwitchTeam`
- `ChangeTeam` Str TeamName
- `SwitchLevel` Str URL
- `ClearProgressMessages`
- `SetProgressMessage` Byte MessageType Str Message Str Title
- `SetProgressTime` Float T
- `CauseEvent` Name EventName
- `CE` Name EventName
- `ListConsoleEvents`
- `ListCE`
- `SaveClassConfig` Str ClassName
- `SaveActorConfig` Name actorName
- `ConsoleKey` Name Key
- `SendToConsole` Str Command
- `PathStep` Int Cnt
- `PathChild` Int Cnt
- `PathClear`
- `BugItGo` Float X Float Y Float Z Int Pitch Int Yaw Int Roll
- `BugIt` Str ScreenShotDescription

## Admin (Engine.u)

- `Admin` Str CommandLine
- `KickBan` Str S
- `Kick` Str S
- `PlayerList`
- `RestartMap`
- `Switch` Str URL

## CheatManager (Engine.u)

- `ListDynamicActors`
- `FreezeFrame` Float delay
- `WriteToLog` Str Param
- `KillViewedActor`
- `Teleport`
- `ChangeSize` Float F
- `EndPath`
- `Amphibious`
- `Fly`
- `Walk`
- `Ghost`
- `AllAmmo`
- `God`
- `AffectedByHitEffects`
- `SloMo` Float T
- `SetJumpZ` Float F
- `SetGravity` Float F
- `SetSpeed` Float F
- `KillAll` Class aClass
- `KillPawns` Class PawnClass
- `Avatar` Name ClassName
- `Summon` Str ClassName
- `GiveWeapon` Str WeaponClassStr
- `PlayersOnly`
- `RememberSpot`
- `ViewSelf` Bool bQuiet
- `ViewPlayer` Str S
- `ViewActor` Name actorName
- `ViewFlag`
- `ViewBot`
- `ViewClass` Class aClass
- `Loaded`
- `AllWeapons`
- `StreamLevelIn` Name PackageName
- `OnlyLoadLevel` Name PackageName
- `StreamLevelOut` Name PackageName
- `ToggleDebugCamera`
- `TestLevel`

## DebugCameraController (Engine.u)

- `SetFreezeRendering`
- `MoreSpeed`
- `NormalSpeed`

## HUD (Engine.u)

- `ToggleHUD`
- `ShowHUD`
- `ShowScores`
- `SetShowScores` Bool bNewValue
- `ShowDebug` Name DebugType
- `FXPlay` Class aClass Str FXAnimPath
- `FXStop` Class aClass

## Input (Engine.u)

- `SetBind` Name BindName Str Command

## PlayerInput (Engine.u)

- `InvertMouse`
- `InvertTurn`
- `SetSensitivity` Float F
- `Jump`
- `SmartJump`
- `ClearSmoothing`

## GameInfo (Engine.u)

- `KillBots`

## GameUISceneClient (Engine.u)

- `ShowDockingStacks`
- `ShowRenderBounds`
- `ShowMenuStates`
- `ToggleDebugInput` Bool bEnable
- `CreateMenu` Class SceneClass Int PlayerIndex
- `OpenMenu` Str MenuPath Int PlayerIndex
- `CloseMenu` Name SceneName
- `ShowDataStoreField` Str DataStoreMarkup
- `RefreshFormatting`
- `ShowDataStores` Bool bVerbose
- `ShowMenuProgression`

## GameViewportClient (Engine.u)

- `DebugCreatePlayer` Int ControllerId
- `SSSwapControllers`
- `DebugRemovePlayer` Int ControllerId
- `SetSplit` Int Mode
- `ShowTitleSafeArea`
- `SetConsoleTarget` Int PlayerIndex

## InventoryManager (Engine.u)

- `DumpWeaponStats`
