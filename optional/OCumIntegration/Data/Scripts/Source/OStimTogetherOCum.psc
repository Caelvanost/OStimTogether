ScriptName OStimTogetherOCum extends Quest

; OStim Together v0.31.5 - optional OCum Ascended integration
;
; Appearance authority belongs to the TRUE local PlayerCharacter. The script
; never rebroadcasts state simulated on this client's STR proxy.
;
; OCum's ocum_applied_cum event remains a useful low-latency trigger, but the
; 0.31.4 multiplayer runtime test proved it is not reliable enough as the only
; trigger. OStim's thread-0 legacy start/end events are therefore used to keep a
; bounded local-player poll alive for the duration of the scene. The poll reads
; OStim's own equip-object state and asks the C++ core to capture/refresh the
; current RaceMenu overlays.

String AddonEventName = "ostimtogether_addon"
String AddonChannel = "ocum"
String OverlayMarker = "CumOverlays"
String VaginalObject = "ocumvagmesh"
String AnalObject = "ocumanmesh"

Bool ScenePollActive = False
Float ScenePollInterval = 0.50

Event OnInit()
    RegisterIntegration()
EndEvent

Function RegisterIntegration()
    UnregisterForModEvent("ocum_applied_cum")
    UnregisterForModEvent("ostim_start")
    UnregisterForModEvent("ostim_end")
    UnregisterForUpdate()
    ScenePollActive = False

    if !Game.IsPluginInstalled("OCum.esp")
        Debug.Trace("[OStimTogetherOCum] OCum.esp not installed; integration disabled")
        return
    endif

    RegisterForModEvent("ocum_applied_cum", "OnOCumApplied")
    RegisterForModEvent("ostim_start", "OnOStimStart")
    RegisterForModEvent("ostim_end", "OnOStimEnd")
    Debug.Trace("[OStimTogetherOCum] v0.31.5 registered; live scene poll enabled")
EndFunction

; OStim thread 0 emits this standard mod event on every local player scene,
; including a remote mirror whose real local PlayerCharacter participates.
Event OnOStimStart(String eventName, String strArg, Float numArg, Form sender)
    ScenePollActive = True
    UnregisterForUpdate()

    ; Let OStim finish START/ChangeNode/body setup before the first capture.
    RegisterForSingleUpdate(0.25)
    Debug.Trace("[OStimTogetherOCum] OStim start; live poll armed")
EndEvent

Event OnOStimEnd(String eventName, String strArg, Float numArg, Form sender)
    ScenePollActive = False
    UnregisterForUpdate()
    Debug.Trace("[OStimTogetherOCum] OStim end; live poll stopped")
EndEvent

Event OnUpdate()
    if !ScenePollActive
        return
    endif

    Actor Target = Game.GetPlayer()
    if Target != None
        SyncCurrentState(Target, "scene-poll")
    endif

    if ScenePollActive
        RegisterForSingleUpdate(ScenePollInterval)
    endif
EndEvent

; OCum Ascended sends this custom event immediately before it applies the
; requested area. Its argument order is:
; orgasmer, target, amountML, area, sceneID.
Event OnOCumApplied(Form OrgasmerForm, Form TargetForm, Float AmountML, String Area, String SceneID)
    Actor Target = TargetForm as Actor

    ; Never let the locally simulated STR proxy become an appearance authority.
    if Target == None || Target != Game.GetPlayer()
        return
    endif

    Debug.Trace("[OStimTogetherOCum] applied event area=" + Area + " scene=" + SceneID + " amount=" + AmountML)

    ; SendCumAppliedEvents() runs before OCum's actual ApplyCumX() call.
    ; Keep the old bounded probes for low latency; the scene poll is the robust
    ; fallback if this custom event is delayed, missed, or fires before the
    ; equip-object / overlay state becomes observable.
    Utility.Wait(0.15)
    SyncCurrentState(Target, Area)

    Utility.Wait(0.35)
    SyncCurrentState(Target, Area)

    Utility.Wait(0.75)
    SyncCurrentState(Target, Area)

    Utility.Wait(1.25)
    SyncCurrentState(Target, Area)
EndEvent

Function SyncCurrentState(Actor Target, String Area)
    bool HasVaginalMesh = false
    bool HasAnalMesh = false
    bool OStimVaginalMesh = false
    bool OStimAnalMesh = false
    bool ArmorVaginalMesh = false
    bool ArmorAnalMesh = false
    float VaginalState = 0.0
    float AnalState = 0.0
    Armor VaginalMeshArmor = None
    Armor AnalMeshArmor = None

    if Target == None
        return
    endif

    ; Ask the generic Core to capture every RaceMenu overlay slot whose current
    ; texture belongs to this marker and send its COMPLETE live state. Core also
    ; reapplies those exact properties to the local live overlay geometry. This
    ; is important during OStim body/3D rebuilds where SKEE can retain property
    ; values while the visible overlay geometry is stale or culled.
    Target.SendModEvent(AddonEventName, "OVR|" + AddonChannel + "|" + OverlayMarker, 0.0)

    ; OStim equip-object state is the primary source of truth. The 0.31.4 test
    ; proved the vaginal/anal object can be visibly active while ordinary Skyrim
    ; inventory IsWorn() still reports the backing armor as not worn.
    OStimVaginalMesh = OActor.IsObjectEquipped(Target, VaginalObject)
    OStimAnalMesh = OActor.IsObjectEquipped(Target, AnalObject)

    ; Keep the validated OCum armor forms only as a fallback for OCum/OStim
    ; paths that expose them as conventional equipped armor.
    VaginalMeshArmor = Game.GetFormFromFile(0x00000F37, "OCum.esp") as Armor
    AnalMeshArmor = Game.GetFormFromFile(0x00000F3B, "OCum.esp") as Armor

    if VaginalMeshArmor != None
        ArmorVaginalMesh = Target.IsEquipped(VaginalMeshArmor)
    endif

    if AnalMeshArmor != None
        ArmorAnalMesh = Target.IsEquipped(AnalMeshArmor)
    endif

    HasVaginalMesh = OStimVaginalMesh || ArmorVaginalMesh
    HasAnalMesh = OStimAnalMesh || ArmorAnalMesh

    if HasVaginalMesh
        VaginalState = 1.0
    endif

    if HasAnalMesh
        AnalState = 1.0
    endif

    Target.SendModEvent(AddonEventName, "OBJ|" + AddonChannel + "|" + VaginalObject, VaginalState)
    Target.SendModEvent(AddonEventName, "OBJ|" + AddonChannel + "|" + AnalObject, AnalState)

    ; Avoid flooding Papyrus. The recurring scene-poll path is intentionally
    ; silent; event-triggered probes retain detailed traces for diagnostics.
    if Area != "scene-poll"
        Debug.Trace("[OStimTogetherOCum] sync area=" + Area + " vagMesh=" + HasVaginalMesh + " analMesh=" + HasAnalMesh + " ostimVag=" + OStimVaginalMesh + " armorVag=" + ArmorVaginalMesh + " ostimAnal=" + OStimAnalMesh + " armorAnal=" + ArmorAnalMesh)
    endif
EndFunction
