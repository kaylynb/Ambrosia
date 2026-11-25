#include "extension/windows.h"

#include "extension/window_internal.h"

#include <algorithm>
#include <cmath>

namespace dx::extension {
    void UWindow::updateLayout()
    {
        if (!visible()) {
            return;
        }
        UWindow* owner = parent();
        if (owner) {
            uint8_t halign = window::winHAlign(this);
            uint8_t valign = window::winVAlign(this);
            float leftMargin = window::hMargin0(this);
            float rightMargin = window::hMargin1(this);
            float topMargin = window::vMargin0(this);
            float bottomMargin = window::vMargin1(this);
            float parentWidth = owner->width();
            float parentHeight = owner->height();
            bool parentIsRoot = owner == state().root;
            float rootWidth = extendedVirtualWidth();
            if (halign == HALIGN_Full || valign == VALIGN_Full) {
                float newWidth = width();
                float newHeight = height();
                if (halign == HALIGN_Full) {
                    x() = 0;
                    newWidth = std::max((parentIsRoot ? rootWidth : parentWidth) - leftMargin - rightMargin, 0.0f);
                }
                if (valign == VALIGN_Full) {
                    y() = 0;
                    newHeight = std::max(parentHeight - topMargin - bottomMargin, 0.0f);
                }
                if (width() != newWidth || height() != newHeight) {
                    width() = newWidth;
                    height() = newHeight;
                    window::bNeedsReconfigure(this) = 1;
                }
            }
            float offsetX = 0;
            if (parentIsRoot) {
                float extra = std::max(rootWidth - parentWidth, 0.0f);
                offsetX = -std::round(extra * 0.5f);
                parentWidth += extra;
            }
            float px = x();
            float py = y();
            if (!window::bConfigured(this)) {
                // XWindow::ResizeChild: a window its
                // parent does not place itself sits by its alignment and first
                // margins; the second margins only matter for HALIGN_Full and
                // VALIGN_Full. The result is the window's x and y, which is what
                // scripts read (dragging a menu calls SetPos(x + dx, y + dy)).
                if (halign == HALIGN_Center) {
                    px = static_cast<float>(static_cast<int32_t>((parentWidth - width()) * 0.5f)) + leftMargin;
                } else if (halign == HALIGN_Right) {
                    px = parentWidth - width() - leftMargin;
                } else {
                    px = leftMargin;
                }
                if (valign == VALIGN_Center) {
                    py = static_cast<float>(static_cast<int32_t>((parentHeight - height()) * 0.5f)) + topMargin;
                } else if (valign == VALIGN_Bottom) {
                    py = parentHeight - height() - topMargin;
                } else {
                    py = topMargin;
                }
                x() = px;
                y() = py;
            }
            usedX = px + offsetX;
            usedY = py;
        } else {
            float w = virtualWidth();
            float h = virtualHeight();
            if (width() != w || height() != h) {
                x() = 0;
                y() = 0;
                width() = w;
                height() = h;
                window::bConfigured(this) = 1;
                window::bNeedsReconfigure(this) = 1;
            }
            usedX = std::round((state().viewportWidth - w * virtualScale()) * 0.5f) / virtualScale();
            usedY = std::round((state().viewportHeight - h * virtualScale()) * 0.5f) / virtualScale();
        }

        bool reconfigured = window::bNeedsReconfigure(this) != 0;
        if (reconfigured) {
            window::bNeedsReconfigure(this) = 0;
            configurationChanged();
        }

        for (UWindow* child = firstChild(); child; child = child->nextSibling()) {
            bool needsQuery = window::bNeedsQuery(child) != 0;
            window::bNeedsQuery(child) = 0;
            if (needsQuery || ((reconfigured || child->firstDraw) && !window::bConfigured(child))) {
                float newWidth = window::hardcodedWidth(child);
                float newHeight = window::hardcodedHeight(child);
                if (!child->fixedWidth && !child->fixedHeight) {
                    child->queryPreferredSize(newWidth, newHeight);
                } else if (child->fixedWidth && !child->fixedHeight) {
                    newHeight = child->queryPreferredHeight(newWidth);
                } else if (child->fixedHeight && !child->fixedWidth) {
                    newWidth = child->queryPreferredWidth(newHeight);
                }
                if (child->width() != newWidth || child->height() != newHeight || child->firstDraw) {
                    child->width() = newWidth;
                    child->height() = newHeight;
                    window::bNeedsReconfigure(child) = 1;
                }
            }
        }

        UWindow* child = firstChild();
        UWindow* previous = nullptr;
        while (child) {
            UWindow* following = child->nextSibling();
            child->updateLayout();
            UWindow* next = nextChild(this, child, following, previous);
            if (attachedTo(child, this)) {
                previous = child;
            }
            child = next;
        }

        if (firstDraw) {
            firstDraw = false;
            windowReady();
        }
    }

    void UWindow::setWidth(float value)
    {
        window::hardcodedWidth(this) = value;
        fixedWidth = true;
        window::bNeedsReconfigure(this) = 1;
        askParentForReconfigure();
    }

    void UWindow::setHeight(float value)
    {
        window::hardcodedHeight(this) = value;
        fixedHeight = true;
        window::bNeedsReconfigure(this) = 1;
        askParentForReconfigure();
    }

    void UWindow::configureChild(float newX, float newY, float newWidth, float newHeight)
    {
        if (fixedWidth) {
            newWidth = window::hardcodedWidth(this);
        }
        if (fixedHeight) {
            newHeight = window::hardcodedHeight(this);
        }
        if (UWindow* owner = parent()) {
            if (window::winHAlign(this) == HALIGN_Full) {
                newX = 0;
                float available = owner == state().root ? extendedVirtualWidth() : owner->width();
                newWidth = std::max(available - window::hMargin0(this) - window::hMargin1(this), 0.0f);
            }
            if (window::winVAlign(this) == VALIGN_Full) {
                newY = 0;
                newHeight = std::max(owner->height() - window::vMargin0(this) - window::vMargin1(this), 0.0f);
            }
        }
        x() = newX;
        y() = newY;
        window::bConfigured(this) = 1;
        window::bNeedsQuery(this) = 0;
        width() = newWidth;
        height() = newHeight;
        window::bNeedsReconfigure(this) = 1;
    }

    // XWindow::QueryPreferredSize, which the width and
    // height queries also go through. A hardcoded size counts as specified. An
    // unspecified dimension reaches ParentRequestedPreferredSize as -1, and if it
    // is still negative afterwards the window's background texture size is used,
    // or failing that its current size.
    void UWindow::preferredSize(bool widthSpecified, float& preferredWidth, bool heightSpecified, float& preferredHeight)
    {
        if (!widthSpecified && fixedWidth) {
            widthSpecified = true;
            preferredWidth = window::hardcodedWidth(this);
        }
        if (!heightSpecified && fixedHeight) {
            heightSpecified = true;
            preferredHeight = window::hardcodedHeight(this);
        }
        if (widthSpecified && heightSpecified) {
            return;
        }
        if (!widthSpecified) {
            preferredWidth = -1;
        }
        if (!heightSpecified) {
            preferredHeight = -1;
        }
        float givenWidth = preferredWidth;
        float givenHeight = preferredHeight;
        parentRequestedPreferredSize(widthSpecified, preferredWidth, heightSpecified, preferredHeight);
        // Text measures its contents, but a specified dimension is a constraint. In
        // particular AlignWindow positions the next column using the label's fixed width.
        if (widthSpecified) {
            preferredWidth = givenWidth;
        }
        if (heightSpecified) {
            preferredHeight = givenHeight;
        }
        UObject* background = window::Background(this);
        if (preferredWidth < 0) {
            preferredWidth = background ? textureWidth(background) : width();
        }
        if (preferredHeight < 0) {
            preferredHeight = background ? textureHeight(background) : height();
        }
    }

    void UWindow::queryPreferredSize(float& preferredWidth, float& preferredHeight)
    {
        preferredSize(false, preferredWidth, false, preferredHeight);
        window::lastQueryWidth(this) = preferredWidth;
        window::lastQueryHeight(this) = preferredHeight;
    }

    float UWindow::queryPreferredWidth(float queryHeight)
    {
        float result = 0;
        preferredSize(false, result, true, queryHeight);
        window::lastQueryWidth(this) = result;
        return result;
    }

    float UWindow::queryPreferredHeight(float queryWidth)
    {
        float result = 0;
        preferredSize(true, queryWidth, false, result);
        window::lastQueryHeight(this) = result;
        return result;
    }

    void UWindow::askParentForReconfigure()
    {
        for (UWindow* current = parent(); current; current = current->parent()) {
            window::bNeedsReconfigure(current) = 1;
            if (current->childRequestedReconfiguration(this)) {
                break;
            }
        }
    }

    void UWindow::askParentToShowArea(float areaX, float areaY, float areaWidth, float areaHeight)
    {
        if (UWindow* owner = parent()) {
            owner->childRequestedShowArea(this, areaX, areaY, areaWidth, areaHeight);
        }
    }
}
