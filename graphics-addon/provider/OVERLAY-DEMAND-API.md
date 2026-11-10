# Overlay demand API v1

Resolve this optional, undecorated export from the ReShade module supplied to
the add-on's `AddonInit` entry point:

```cpp
extern "C" uint32_t WINAPI TextureQuestSetOverlayCallbackDemandV1(
    void *module, void *callback, uint32_t requested);
```

`module` is the registered add-on module handle. `callback` is the exact
`reinterpret_cast<void *>(&Callback)` passed when registering that add-on's
`reshade_overlay` event. Zero means the callback currently has no GUI to draw;
any nonzero value means it needs GUI drawing. The call returns 1 when the
registered pair is found, including an unchanged request; it returns 0 for
unknown, wrong-owner or null identities. The provider compares identities and
does not dereference either client pointer.

Register the event normally, then set demand from the saved display preference.
Update demand when that preference changes. No per-frame request is needed.
Registration, duplicate registration and re-registration always enable demand
by default. Unregistration removes the pair. A caller must reapply its current
preference after registering again. Other add-ons remain demanding unless they
explicitly use this API, so an unrelated overlay cannot be accidentally hidden.
If the export is absent or rejects the identity, leave the event registered and
retain the ordinary overlay behavior.

Demand controls only the existing GUI early-return decision. An enabled Home
menu, splash, screenshot notification, statistics widget, texture preview or
another demanding overlay still runs the GUI and dispatches overlay callbacks
normally. Each callback must retain its own local visibility check. The API
does not suppress particular callbacks or change input handling.

Demand changes are synchronized through a separate mutex-protected registry;
GUI admission reads an atomic active count. Multiple runtime threads may read
or change demand safely without changing ReShade's callback vectors. A change
may take effect on the next frame if another thread has already checked demand.
Event registration, unregistration and dispatch themselves retain ReShade's
existing lifecycle requirements. A single callback's demand is global across
runtime instances, so clients with per-runtime visibility must request demand
while any runtime needs that callback.
