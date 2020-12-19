// yabridge: a Wine VST bridge
// Copyright (C) 2020  Robbert van der Helm
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#pragma once

#include <pluginterfaces/gui/iplugview.h>

#include "../../common.h"
#include "../base.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnon-virtual-dtor"

/**
 * Wraps around `IPlugView` for serialization purposes. This is instantiated as
 * part of `Vst3PlugViewProxy`.
 */
class YaPlugView : public Steinberg::IPlugView {
   public:
    /**
     * These are the arguments for creating a `YaPlugView`.
     */
    struct ConstructArgs {
        ConstructArgs();

        /**
         * Check whether an existing implementation implements `IPlugView` and
         * read arguments from it.
         */
        ConstructArgs(Steinberg::IPtr<Steinberg::FUnknown> object);

        /**
         * Whether the object supported this interface.
         */
        bool supported;

        template <typename S>
        void serialize(S& s) {
            s.value1b(supported);
        }
    };

    /**
     * Instantiate this instance with arguments read from another interface
     * implementation.
     */
    YaPlugView(const ConstructArgs&& args);

    inline bool supported() const { return arguments.supported; }

    /**
     * Message to pass through a call to
     * `IPlugView::isPlatformTypeSupported(type)` to the Wine plugin host. We
     * will of course change `kPlatformStringLinux` for `kPlatformStringWin`,
     * because why would a Windows VST3 plugin have X11 support? (and how would
     * that even work)
     */
    struct IsPlatformTypeSupported {
        using Response = UniversalTResult;

        native_size_t owner_instance_id;

        std::string type;

        template <typename S>
        void serialize(S& s) {
            s.value8b(owner_instance_id);
            s.text1b(type, 128);
        }
    };

    virtual tresult PLUGIN_API
    isPlatformTypeSupported(Steinberg::FIDString type) override = 0;

    /**
     * Message to pass through a call to `IPlugView::attached(parent, type)` to
     * the Wine plugin host. Like mentioned above we will substitute
     * `kPlatformStringWin` for `kPlatformStringLinux`.
     */
    struct Attached {
        using Response = UniversalTResult;

        native_size_t owner_instance_id;

        /**
         * The parent handle passed by the host. This will be an
         * `xcb_window_id`, and we'll embed the Wine window into it ourselves.
         */
        native_size_t parent;
        std::string type;

        template <typename S>
        void serialize(S& s) {
            s.value8b(owner_instance_id);
            s.value8b(parent);
            s.text1b(type, 128);
        }
    };

    virtual tresult PLUGIN_API attached(void* parent,
                                        Steinberg::FIDString type) override = 0;
    virtual tresult PLUGIN_API removed() override = 0;
    virtual tresult PLUGIN_API onWheel(float distance) override = 0;
    virtual tresult PLUGIN_API onKeyDown(char16 key,
                                         int16 keyCode,
                                         int16 modifiers) override = 0;
    virtual tresult PLUGIN_API onKeyUp(char16 key,
                                       int16 keyCode,
                                       int16 modifiers) override = 0;
    virtual tresult PLUGIN_API getSize(Steinberg::ViewRect* size) override = 0;
    virtual tresult PLUGIN_API
    onSize(Steinberg::ViewRect* newSize) override = 0;
    virtual tresult PLUGIN_API onFocus(TBool state) override = 0;
    virtual tresult PLUGIN_API
    setFrame(Steinberg::IPlugFrame* frame) override = 0;
    virtual tresult PLUGIN_API canResize() override = 0;
    virtual tresult PLUGIN_API
    checkSizeConstraint(Steinberg::ViewRect* rect) override = 0;

   protected:
    ConstructArgs arguments;
};

#pragma GCC diagnostic pop
