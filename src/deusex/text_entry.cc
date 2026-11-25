#include "deusex/text_entry.h"

#include "deusex/script.h"
#include "deusex/text_input.h"
#include "engine/input.h"
#include "engine/props_extension.h"
#include "extension/extension.h"
#include "extension/windows.h"

namespace dx::textentry {
    namespace {
        // The login and ATM screens keep their secret field in these members.
        bool isSecret(extension::UWindow* field)
        {
            for (extension::UWindow* owner = field->parent(); owner; owner = owner->parent()) {
                for (const char* member : { "editPassword", "editPIN" }) {
                    if (owner->cls->findProperty(Name(member)) && script::object(owner, member) == field) {
                        return true;
                    }
                }
            }
            return false;
        }
    }

    std::optional<Request> requestFor(UObject* window)
    {
        auto* edit = cast<extension::UEditWindow>(window);
        if (!edit || edit->destroyed || !editwindow::bEditable(edit) || !edit->sensitive() || !edit->effectivelyVisible()) {
            return std::nullopt;
        }
        Request request;
        request.field = edit;
        request.text = textinput::toUtf8(edit->text());
        request.maxLength = editwindow::maxSize(edit);
        request.password = isSecret(edit);
        request.multiline = !editwindow::bSingleLine(edit);
        return request;
    }

    void deliver(const Request& request, const std::string& typed)
    {
        auto* edit = cast<extension::UEditWindow>(request.field);
        if (!edit || edit->destroyed) {
            return;
        }
        int32_t length = edit->textLength();
        editwindow::selectStart(edit) = 0;
        editwindow::selectEnd(edit) = length;
        editwindow::insertPos(edit) = length;
        if (!edit->insertText(textinput::fromUtf8(typed), true, false) || edit->destroyed) {
            return;
        }
        if (editwindow::bSingleLine(edit)) {
            edit->virtualKeyPressed(IK_Enter, false);
        }
    }
}
