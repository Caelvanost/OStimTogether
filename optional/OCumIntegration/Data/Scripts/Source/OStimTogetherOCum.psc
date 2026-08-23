ScriptName OStimTogetherOCum extends Quest

; OStim Together v0.31.0 - optional OCum Ascended integration
;
; This script intentionally contains ALL OCum-specific knowledge.
; OStimTogether.dll only sees opaque addon channels / texture markers /
; OStim equip-object type strings.

String AddonEventName = "ostimtogether_addon"
String AddonChannel = "ocum"
String OverlayMarker = "CumOverlays"
String VaginalObject = "ocumvagmesh"
String AnalObject = "ocumanmesh"

Event OnInit()
    RegisterIntegration()
EndEvent

Function RegisterIntegration()
    UnregisterForModEvent("ocum_applied_cum")

    if !Game.IsPluginInstalled("OCum.esp")
        Debug.Trace("[OStimTogetherOCum] OCum.esp not installed; integration disabled")
        return
    endif

    RegisterForModEvent("ocum_applied_cum", "OnOCumApplied")
    Debug.Trace("[OStimTogetherOCum] v0.31.0 registered for ocum_applied_cum")
EndFunction

; OCum Ascended sends this custom event immediately before it applies the
; requested area. Its argument order is:
; orgasmer, target, amountML, area, sceneID.
Event OnOCumApplied(Form OrgasmerForm, Form TargetForm, Float AmountML, String Area, String SceneID)
    Actor Target = TargetForm as Actor

    ; Appearance authority belongs to the true local player. Never rebroadcast
    ; effects OCum happens to apply to this client's STR proxy of the other
    ; player, otherwise both clients become competing authorities.
    if Target == None || Target != Game.GetPlayer()
        return
    endif

    Debug.Trace("[OStimTogetherOCum] applied event area=" + Area + " scene=" + SceneID + " amount=" + AmountML)

    ; SendCumAppliedEvents() runs before OCum's actual ApplyCumX() call.
    ; Probe several bounded times instead of polling for the entire scene.
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

    ; Ask the generic Core to capture every RaceMenu overlay slot whose
    ; current texture belongs to this marker and send its COMPLETE live state.
    ; This covers Face / Body / Hands / Feet without hard-coding nodes here.
    Target.SendModEvent(AddonEventName, "OVR|" + AddonChannel + "|" + OverlayMarker, 0.0)

    ; OCum's vaginal/anal meshes are OStim equip objects. OStim's own
    ; IsObjectEquipped() is the primary source of truth because an equip object
    ; can be active even when Skyrim inventory queries do not expose the armor
    ; as a conventional worn item on that exact frame.
    OStimVaginalMesh = OActor.IsObjectEquipped(Target, VaginalObject)
    OStimAnalMesh = OActor.IsObjectEquipped(Target, AnalObject)

    ; Keep the validated OCum armor forms as a fallback. Older OCum/OStim paths
    ; have also been observed to expose the live state here. The authoritative
    ; state sent over the network is the OR of both representations.
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

    Debug.Trace("[OStimTogetherOCum] sync area=" + Area + " vagMesh=" + HasVaginalMesh + " analMesh=" + HasAnalMesh + " ostimVag=" + OStimVaginalMesh + " armorVag=" + ArmorVaginalMesh + " ostimAnal=" + OStimAnalMesh + " armorAnal=" + ArmorAnalMesh)
EndFunction
