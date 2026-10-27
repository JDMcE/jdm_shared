#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace jdm
{

// Snapshot-based state history + A/B comparison (v0.8).
//
// Replaces the juce::UndoManager wiring from v0.2: APVTS can only bind one
// UndoManager for its whole lifetime and replaceState() wipes it, which
// makes "independent undo/redo history per A/B state" impossible on top of
// it. Instead, every edit burst is committed as a full state snapshot
// (the tree is tiny), and each A/B slot owns its snapshot stacks outright —
// switching slots or loading presets never bleeds history across.
//
// Parameter changes are detected through AudioProcessorListener (which may
// fire on the audio thread, so the detection path is atomics only); a
// message-thread timer coalesces a burst of changes into one transaction
// once it has been quiet for a short while. Non-parameter state edits
// (e.g. a committed MIDI Learn mapping) are flagged explicitly via
// noteNonParameterChange().
//
// Host automation is not an edit: played back, it would fill the history
// with steps nobody made. With setOnlyGesturesAreEdits (true) a parameter
// change counts only inside a change gesture (a control being dragged, here
// or in the host's own UI); automation and other ungestured sets don't, but
// they are taken into the slot's current state once quiet, so the next undo
// takes back the edit and not the automation as well.
class StateManager : private juce::AudioProcessorListener,
                     private juce::Timer
{
public:
    StateManager (juce::AudioProcessor& p, juce::AudioProcessorValueTreeState& s,
                  const juce::Identifier& sessionTypeIn)
        : sessionType (sessionTypeIn), processor (p), apvts (s)
    {
        for (auto& slot : slots)
            slot.current = apvts.copyState();

        processor.addListener (this);
        startTimer (150);
    }

    ~StateManager() override
    {
        processor.removeListener (this);
    }

    // Called after a snapshot is applied wholesale (undo/redo/slot switch/
    // preset load) so the owner can refresh caches built from the tree
    // (MIDI CC map etc.).
    std::function<void()> onStateReapplied;

    // Off by default (every parameter change is an edit, as before).
    void setOnlyGesturesAreEdits (bool on) { onlyGestures.store (on); }

    // Undo/redo (active slot) --------------------------------------------------

    bool canUndo() const { return dirty.load() || ! slots[active].undoStack.empty(); }
    bool canRedo() const { return ! slots[active].redoStack.empty(); }

    void undo()
    {
        commitPendingChanges();
        auto& slot = slots[active];
        if (slot.undoStack.empty())
            return;

        slot.redoStack.push_back (slot.current);
        slot.current = slot.undoStack.back();
        slot.undoStack.pop_back();
        applyTree (slot.current);
    }

    void redo()
    {
        commitPendingChanges();
        auto& slot = slots[active];
        if (slot.redoStack.empty())
            return;

        slot.undoStack.push_back (slot.current);
        slot.current = slot.redoStack.back();
        slot.redoStack.pop_back();
        applyTree (slot.current);
    }

    // Folds any pending edit burst into one undoable transaction. The timer
    // calls this automatically; it is public so tests (and anything about to
    // read slot state) can force the point-in-time.
    void commitPendingChanges()
    {
        if (! dirty.exchange (false))
        {
            if (unrecorded.exchange (false))
                slots[active].current = apvts.copyState(); // silent: automation isn't an edit
            return;
        }
        unrecorded = false;

        auto& slot = slots[active];
        auto now = apvts.copyState();
        if (now.isEquivalentTo (slot.current))
            return;

        pushUndo (slot, slot.current);
        slot.redoStack.clear();
        slot.current = std::move (now);
    }

    // Audio-thread-safe: state got edited outside the parameter system.
    void noteNonParameterChange()
    {
        lastChangeMs.store (juce::Time::getMillisecondCounter());
        dirty.store (true);
    }

    // A/B ----------------------------------------------------------------------

    int getActiveSlot() const { return active; }

    void setActiveSlot (int newSlot)
    {
        newSlot = juce::jlimit (0, (int) slots.size() - 1, newSlot);
        if (newSlot == active)
            return;

        commitPendingChanges();
        slots[active].current = apvts.copyState();
        active = newSlot;
        applyTree (slots[active].current);
    }

    // Copies the active slot's state over the other slot. The copy is
    // undoable *in the other slot's own history*.
    void copyActiveToOther()
    {
        commitPendingChanges();
        auto& src = slots[active];
        auto& dst = slots[1 - active];
        src.current = apvts.copyState();

        pushUndo (dst, dst.current);
        dst.redoStack.clear();
        dst.current = src.current.createCopy();
    }

    // Preset identity ------------------------------------------------------------
    // Stored as a property on the state root so it rides along with
    // sessions, snapshots, and undo (undoing past a preset load restores
    // the previous name).

    juce::String getCurrentPresetName() const
    {
        return apvts.state.getProperty (presetNameProperty, juce::String()).toString();
    }

    void setCurrentPresetName (const juce::String& name)
    {
        commitPendingChanges();
        apvts.state.setProperty (presetNameProperty, name, nullptr);
        slots[active].current = apvts.copyState(); // silent: naming isn't an edit
    }

    // Whole-state application (preset load): one undoable transaction in the
    // active slot's history.
    void applyState (const juce::ValueTree& newState)
    {
        commitPendingChanges();
        auto& slot = slots[active];
        slot.current = apvts.copyState();

        pushUndo (slot, slot.current);
        slot.redoStack.clear();
        slot.current = newState.createCopy();
        applyTree (slot.current);
    }

    // Session persistence ---------------------------------------------------------
    // Both slots + which is active. Undo stacks are session-local by design.

    // Product-specific root tag for saved sessions (e.g. "ZenithSession").
    const juce::Identifier sessionType;

    juce::ValueTree saveSession()
    {
        commitPendingChanges();
        slots[active].current = apvts.copyState(); // catch non-parameter stragglers

        juce::ValueTree session (sessionType);
        session.setProperty ("activeSlot", active, nullptr);
        for (auto& slot : slots)
            session.appendChild (slot.current.createCopy(), nullptr);
        return session;
    }

    bool loadSession (const juce::ValueTree& session)
    {
        if (! session.hasType (sessionType) || session.getNumChildren() < (int) slots.size())
            return false;

        for (size_t i = 0; i < slots.size(); ++i)
        {
            if (! session.getChild ((int) i).hasType (apvts.state.getType()))
                return false;
            slots[i] = {};
            slots[i].current = session.getChild ((int) i).createCopy();
        }

        active = juce::jlimit (0, (int) slots.size() - 1,
                               (int) session.getProperty ("activeSlot", 0));
        applyTree (slots[active].current);
        return true;
    }

    // Pre-v0.8 sessions/presets carried a single bare state: load it into
    // both slots so A/B starts from the restored sound.
    void loadLegacyState (const juce::ValueTree& state)
    {
        for (auto& slot : slots)
        {
            slot = {};
            slot.current = state.createCopy();
        }
        applyTree (slots[active].current);
    }

private:
    static inline const juce::Identifier presetNameProperty { "presetName" };
    static constexpr size_t maxUndoDepth = 128;

    struct Slot
    {
        juce::ValueTree current;
        std::vector<juce::ValueTree> undoStack, redoStack;
    };

    static void pushUndo (Slot& slot, const juce::ValueTree& snapshot)
    {
        slot.undoStack.push_back (snapshot);
        if (slot.undoStack.size() > maxUndoDepth)
            slot.undoStack.erase (slot.undoStack.begin());
    }

    void applyTree (const juce::ValueTree& tree)
    {
        applying = true;
        apvts.replaceState (tree.createCopy());
        applying = false;
        dirty.store (false);

        if (onStateReapplied)
            onStateReapplied();
    }

    // AudioProcessorListener — parameter changes can arrive on the audio
    // thread (automation, MIDI CC), so only touch atomics here.
    void audioProcessorParameterChanged (juce::AudioProcessor*, int, float) override
    {
        if (applying)
            return;
        if (onlyGestures.load() && openGestures.load() <= 0)
        {
            unrecorded.store (true);
            return;
        }
        lastChangeMs.store (juce::Time::getMillisecondCounter());
        dirty.store (true);
    }

    void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails&) override {}

    void audioProcessorParameterChangeGestureBegin (juce::AudioProcessor*, int) override { ++openGestures; }

    void audioProcessorParameterChangeGestureEnd (juce::AudioProcessor*, int) override
    {
        if (--openGestures < 0)
            openGestures.store (0); // an end without its begin
    }

    void timerCallback() override
    {
        if ((dirty.load() && juce::Time::getMillisecondCounter() - lastChangeMs.load() > 250)
            || (! dirty.load() && unrecorded.load()))
            commitPendingChanges();
    }

    juce::AudioProcessor& processor;
    juce::AudioProcessorValueTreeState& apvts;

    std::array<Slot, 2> slots;
    int active = 0;

    std::atomic<bool> dirty { false };
    std::atomic<juce::uint32> lastChangeMs { 0 };
    std::atomic<bool> onlyGestures { false };
    std::atomic<int> openGestures { 0 };
    std::atomic<bool> unrecorded { false };   // ungestured changes since the last snapshot
    bool applying = false; // message thread only

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StateManager)
};

} // namespace jdm
