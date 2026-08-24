ScriptName OStimTogetherOCum extends Quest

; OStim Together v0.36.0 - optional OCum Ascended integration
;
; Appearance authority belongs to the TRUE local PlayerCharacter. OCum and
; RaceMenu exclusively own that actor's local overlay rendering. This script
; only mirrors equip-object state; the C++ core reads CumOverlays without ever
; asking RaceMenu to rebuild/relink/refresh the local player.

String AddonEventName = "ostimtogether_addon"
String AddonChannel = "ocum"
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
    Debug.Trace("[OStimTogetherOCum] v0.36.0 registered; mirror-only mode")
EndFunction

Event OnOStimStart(String eventName, String strArg, Float numArg, Form sender)
    ScenePollActive = True
    UnregisterForUpdate()
    RegisterForSingleUpdate(0.25)
    Debug.Trace("[OStimTogetherOCum] OStim start; object-state poll armed")
EndEvent

Event OnOStimEnd(String eventName, String strArg, Float numArg, Form sender)
    ScenePollActive = False
    UnregisterForUpdate()
    Debug.Trace("[OStimTogetherOCum] OStim end; object-state poll stopped")
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

Event OnOCumApplied(Form OrgasmerForm, Form TargetForm, Float AmountML, String Area, String SceneID)
    Actor Target = TargetForm as Actor

    if Target == None || Target != Game.GetPlayer()
        return
    endif

    Debug.Trace("[OStimTogetherOCum] applied event area=" + Area + " scene=" + SceneID + " amount=" + AmountML)

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

    ; IMPORTANT: no OVR SendModEvent here. In older builds that event made the
    ; C++ bridge refresh the local RaceMenu geometry every poll. v0.36.0 leaves
    ; the local PlayerCharacter completely OCum/RaceMenu-owned and mirrors the
    ; resulting CumOverlays read-only from C++.

    OStimVaginalMesh = OActor.IsObjectEquipped(Target, VaginalObject)
    OStimAnalMesh = OActor.IsObjectEquipped(Target, AnalObject)

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

    if Area != "scene-poll"
        Debug.Trace("[OStimTogetherOCum] sync area=" + Area + " vagMesh=" + HasVaginalMesh + " analMesh=" + HasAnalMesh + " ostimVag=" + OStimVaginalMesh + " armorVag=" + ArmorVaginalMesh + " ostimAnal=" + OStimAnalMesh + " armorAnal=" + ArmorAnalMesh)
    endif
EndFunction
