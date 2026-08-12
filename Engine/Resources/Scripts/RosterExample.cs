using System;
using System.Collections.Generic;
using LuminaSharp;
using Lumina;

namespace Lumina.Examples;

/// <summary>
/// LIST binding (Composition/Roster.rml uses <c>data-for</c>): the model exposes a list and the document
/// renders one card per item. The card count and the per-item values are driven entirely from C#.
///
/// A list is pushed to the view as a SNAPSHOT: mutate the C# list, then call
/// <see cref="ViewModel.NotifyChanged"/> with the list property's name to re-push it. List items are
/// display-only; there is no writeback from a repeated element.
///
/// To run it: add a C# Script component to any entity, point it at <c>Lumina.Examples.RosterExample</c>,
/// and press Play.
/// </summary>
public sealed class RosterExample : EntityScript
{
    [Property(Tooltip = "Roster document.")]
    public string Document = "/Engine/Resources/Content/UI/Examples/Composition/Roster.rml";

    /// <summary>One list item. A plain data class; its [Bind] properties become the {{ member.* }} columns.</summary>
    private sealed class RosterEntry
    {
        [Bind] public string Name { get; set; }
        [Bind] public int Health { get; set; }

        public RosterEntry(string Name, int Health)
        {
            this.Name = Name;
            this.Health = Health;
        }
    }

    private sealed class RosterModel : ViewModel
    {
        private readonly List<RosterEntry> _Members = new()
        {
            new RosterEntry("Aria", 100),
            new RosterEntry("Bex", 100),
            new RosterEntry("Cyl", 100),
        };

        /// <summary>The list bound by data-for="member : Members".</summary>
        [Bind] public IReadOnlyList<RosterEntry> Members => _Members;

        /// <summary>Pulse each member's health out of phase, then re-push the snapshot to the view. Adding or
        /// removing an entry works the same way: mutate _Members, then NotifyChanged, and the card count follows.</summary>
        public void Pulse(float Time)
        {
            for (int i = 0; i < _Members.Count; i++)
            {
                _Members[i].Health = (int)Mathf.Lerp(30.0f, 100.0f, 0.5f * (1.0f + MathF.Sin(Time + i * 0.8f)));
            }
            NotifyChanged(nameof(Members));
        }
    }

    private RosterModel _Model = null!;
    private UIDataModel? _Binding;
    private UIDocument _Roster;

    public override void OnReady()
    {
        _Model = new RosterModel();

        _Binding = World.UI.AddModel("roster", _Model);
        if (!_Binding.IsValid)
        {
            Debug.LogError("RosterExample: failed to register the 'roster' data model.");
            return;
        }

        _Roster = World.UI.LoadDocument(Document);
        if (!_Roster.IsValid)
        {
            Debug.LogError($"RosterExample: failed to load roster document '{Document}'.");
            return;
        }

        _Roster.Show();
        Debug.Log("RosterExample: roster shown. One card per Members item; health pulses live.");
    }

    public override void OnUpdate(float DeltaTime)
    {
        if (_Model.IsBound)
        {
            _Model.Pulse((float)LuminaSharp.Time.Now);
        }
    }

    public override void OnDetach()
    {
        _Roster.Close();
        _Binding?.Dispose();
    }
}
