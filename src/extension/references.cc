#include "extension/windows.h"

#include "core/gc.h"

namespace dx::extension {
    // The window tree's own links (parent, children, siblings, focus, grabs) are script
    // properties and go through UObject::addReferences with the rest of the property data. What
    // follows is the C++ state the window classes keep beside it.

    // The window last clicked, for counting a double click. forgetWindow clears it when that
    // window is destroyed, so holding it never keeps a destroyed window.
    void URootWindow::addReferences(ReferenceVisitor& references)
    {
        UModalWindow::addReferences(references);
        references(lastClickWindow);
    }

    void UButtonWindow::addReferences(ReferenceVisitor& references)
    {
        UTextWindow::addReferences(references);
        for (UObject*& texture : buttonTextures) {
            references(texture);
        }
    }

    // The children placed by the last layout. The list is built again before each use, so a
    // child cleared to None here is never read.
    void UTileWindow::addReferences(ReferenceVisitor& references)
    {
        UWindow::addReferences(references);
        for (Hold& hold : holds) {
            references(hold.window);
        }
    }

    void UListWindow::addReferences(ReferenceVisitor& references)
    {
        UWindow::addReferences(references);
        for (Column& column : columns) {
            references(column.font);
        }
        for (Row& entry : rows) {
            references(entry.clientObject);
        }
    }

    // The sounds and graphics waiting to be shown or on the screen, and the background tiles.
    void UComputerWindow::addReferences(ReferenceVisitor& references)
    {
        UWindow::addReferences(references);
        for (Event& event : queued) {
            references(event.object);
        }
        for (Event& event : display) {
            references(event.object);
        }
        for (UObject*& background : backgrounds) {
            references(background);
        }
    }

    // PushGC keeps whole copies of the GC's script data, fonts and textures included, for
    // PopGC to put back.
    void UGC::addReferences(ReferenceVisitor& references)
    {
        UObject::addReferences(references);
        if (!cls) {
            return;
        }
        for (Saved& saved : savedStates) {
            if (saved.data.size() >= cls->size) {
                gc::addPropertyReferences(cls, saved.data.data(), references);
            }
        }
    }

    namespace {
        // The library's own state: the root window, the shared GCs, the windows destroyed
        // since the last reset (kept until reset hands them to the collector), and the default
        // font and solid texture once loaded. All of them are kept; code that reads them copes
        // with finding None after a collection eliminated one.
        struct StateReferences final : gc::Holder {
            void addReferences(ReferenceVisitor& references) override
            {
                State& s = state();
                references(s.root);
                references(s.gc);
                references(s.windowGc);
                references(s.destroyedWindows);
                references(s.font);
                references(s.solid);
            }
        };
    }

    void registerReferences()
    {
        static StateReferences stateReferences;
        gc::reportsReferences<UWindow, UTabGroupWindow, UModalWindow, URootWindow, UClipWindow, URadioBoxWindow, UTextWindow, UButtonWindow, UToggleWindow, UCheckboxWindow, ULargeTextWindow, UEditWindow, UTextLogWindow, UTileWindow, UBorderWindow, UScaleWindow, UScaleManagerWindow, UScrollAreaWindow, UListWindow, UComputerWindow, UViewportWindow, UGC>();
        registerFlagReferences();
        registerConSysReferences();
    }
}
