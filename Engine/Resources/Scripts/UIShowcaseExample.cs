using System;
using System.Collections.Generic;
using LuminaSharp;
using Lumina;

namespace Lumina.Examples;

// Drives UI/Examples/Showcase.rml. Attach as a C# Script component and press Play.
public sealed class UIShowcaseExample : EntityScript
{
    [Property(Tooltip = "Showcase document to load.")]
    public string Document = "/Engine/Resources/Content/UI/Examples/Showcase.rml";

    // A plain data class; its [Bind] properties become the {{ member.* }} columns of the data-for list.
    private sealed class PartyMember
    {
        [Bind] public string Name { get; set; }
        [Bind] public string Role { get; set; }
        [Bind] public int Health { get; set; }

        public PartyMember(string Name, string Role, int Health)
        {
            this.Name = Name;
            this.Role = Role;
            this.Health = Health;
        }
    }

    private sealed class ShowcaseModel : ViewModel
    {
        private static readonly string[] RosterNames = { "Aria", "Bex", "Cyl", "Dax", "Enna", "Fen" };
        private static readonly string[] RosterRoles = { "scout", "medic", "heavy", "engineer" };

        private string _Title = "UI Showcase";
        private string _Clock = "00:00";
        private string _Status = "nominal";
        private int _Score;
        private float _Ratio = 0.5f;
        private int _Health = 100;
        private bool _Alerted;

        private string _PlayerName = "Commander";
        private string _Passphrase = "hunter2";
        private int _Volume = 70;
        private string _Quality = "high";
        private bool _Fullscreen = true;
        private bool _VSync;
        private string _Difficulty = "normal";
        private string _Notes = "Two-way bound. Type here and the line below follows.";

        private readonly List<PartyMember> _Members = new()
        {
            new PartyMember("Aria", "scout", 100),
            new PartyMember("Bex", "medic", 82),
            new PartyMember("Cyl", "heavy", 64),
        };

        [Bind] public string Title { get => _Title; set => Set(ref _Title, value); }
        [Bind] public string Clock { get => _Clock; set => Set(ref _Clock, value); }
        [Bind] public string Status { get => _Status; set => Set(ref _Status, value); }
        [Bind] public int Score { get => _Score; set => Set(ref _Score, value); }
        [Bind] public float Ratio { get => _Ratio; set => Set(ref _Ratio, value); }

        // Alerted and Status are derived here rather than in RCSS, which has no conditional operators.
        [Bind] public int Health
        {
            get => _Health;
            set
            {
                Set(ref _Health, Math.Clamp(value, 0, 100));
                Alerted = _Health < 40;
                Status = _Health < 40 ? "critical" : "nominal";
            }
        }

        [Bind] public bool Alerted { get => _Alerted; set => Set(ref _Alerted, value); }

        // Get-only, so it is display-only and has to be re-pushed by hand whenever the list changes.
        [Bind] public int MemberCount => _Members.Count;

        // Everything below HAS a setter, which is the only thing that makes a [Bind] two-way.
        [Bind] public string PlayerName { get => _PlayerName; set { Set(ref _PlayerName, value); Debug.Log($"[Showcase] PlayerName -> {value}"); } }
        [Bind] public string Passphrase { get => _Passphrase; set => Set(ref _Passphrase, value); }
        [Bind] public int Volume { get => _Volume; set { Set(ref _Volume, value); Debug.Log($"[Showcase] Volume -> {value}"); } }
        [Bind] public string Quality { get => _Quality; set { Set(ref _Quality, value); Debug.Log($"[Showcase] Quality -> {value}"); } }
        [Bind] public bool Fullscreen { get => _Fullscreen; set { Set(ref _Fullscreen, value); Debug.Log($"[Showcase] Fullscreen -> {value}"); } }
        [Bind] public bool VSync { get => _VSync; set { Set(ref _VSync, value); Debug.Log($"[Showcase] VSync -> {value}"); } }
        [Bind] public string Difficulty { get => _Difficulty; set { Set(ref _Difficulty, value); Debug.Log($"[Showcase] Difficulty -> {value}"); } }
        [Bind] public string Notes { get => _Notes; set => Set(ref _Notes, value); }

        // A list crosses as a one-way snapshot of strings, so mutations must be followed by NotifyChanged.
        [Bind] public IReadOnlyList<PartyMember> Members => _Members;

        [BindCommand]
        public void Damage(int Amount)
        {
            Health -= Amount;
        }

        [BindCommand]
        public void Heal()
        {
            Health = 100;
        }

        [BindCommand]
        public void AddMember()
        {
            if (_Members.Count >= RosterNames.Length)
            {
                return;
            }
            int Index = _Members.Count;
            _Members.Add(new PartyMember(RosterNames[Index], RosterRoles[Index % RosterRoles.Length], 100));
            PushMembers();
        }

        [BindCommand]
        public void RemoveMember()
        {
            if (_Members.Count == 0)
            {
                return;
            }
            _Members.RemoveAt(_Members.Count - 1);
            PushMembers();
        }

        // Called from RML as ApplyPreset('low'): arguments cross as strings and convert to the parameter type.
        [BindCommand]
        public void ApplyPreset(string Preset)
        {
            Debug.Log($"[Showcase] preset -> {Preset}");
            switch (Preset)
            {
                case "low":   Quality = "low";   Volume = 30;  VSync = false; break;
                case "high":  Quality = "high";  Volume = 70;  VSync = true;  break;
                default:      Quality = "ultra"; Volume = 100; VSync = true;  break;
            }
        }

        [BindCommand]
        public void Reset()
        {
            PlayerName = "Commander";
            Volume = 70;
            Quality = "high";
            Fullscreen = true;
            VSync = false;
            Difficulty = "normal";
            Health = 100;
            Score = 0;
        }

        // Set() only pushes on a real change, and RmlUi relayouts the WHOLE document on any change,
        public void Tick(float Time)
        {
            int Seconds = (int)Time;
            Clock = $"{(Seconds / 60) % 60:00}:{Seconds % 60:00}";
            Score = Seconds * 17;

            // so quantising a continuous value to what the eye can resolve is the difference between
            // one reflow per frame and one per visible step.
            Ratio = MathF.Round(0.5f * (1.0f + MathF.Sin(Time * 0.7f)), 2);

            bool bMembersChanged = false;
            for (int i = 0; i < _Members.Count; i++)
            {
                int Next = (int)Mathf.Lerp(35.0f, 100.0f, 0.5f * (1.0f + MathF.Sin(Time + i * 0.9f)));
                if (_Members[i].Health != Next)
                {
                    _Members[i].Health = Next;
                    bMembersChanged = true;
                }
            }

            // A list has no per-item change tracking, so the snapshot is only worth re-pushing on a real edit.
            if (bMembersChanged)
            {
                NotifyChanged(nameof(Members));
            }
        }

        private void PushMembers()
        {
            NotifyChanged(nameof(Members));
            NotifyChanged(nameof(MemberCount));
        }
    }

    private ShowcaseModel _Model = null!;
    private UIDataModel? _Binding;
    private UIDocument _Doc;

    public override void OnReady()
    {
        _Model = new ShowcaseModel();

        // Must precede LoadDocument: RmlUi resolves data bindings while parsing, and never retries.
        _Binding = World.UI.AddModel("showcase", _Model);
        if (!_Binding.IsValid)
        {
            Debug.LogError("UIShowcaseExample: failed to register the 'showcase' data model.");
            return;
        }

        _Doc = World.UI.LoadDocument(Document);
        if (!_Doc.IsValid)
        {
            Debug.LogError($"UIShowcaseExample: failed to load '{Document}'.");
            return;
        }

        // Documents load hidden.
        _Doc.Show();

        // The tabs, sliders and text fields all need a real cursor and keyboard focus.
        World.UI.EnableCursor();
        Debug.Log("UIShowcaseExample: showcase open. Walk the tabs; the last two are live-bound.");
    }

    public override void OnUpdate(float DeltaTime)
    {
        if (_Model.IsBound)
        {
            _Model.Tick((float)LuminaSharp.Time.Now);
        }
    }

    public override void OnDetach()
    {
        // Reverse order, so the managed callbacks outlive the document that can still fire them.
        _Doc.Close();
        _Binding?.Dispose();
        World.UI.DisableCursor();
    }
}
