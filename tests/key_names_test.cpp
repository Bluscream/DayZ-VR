// Host-side test for the loader's hotkey grammar and setting validation
// (loader/key_names.hpp, loader/settings_check.hpp). Built with g++ by build.sh.
#include "../loader/key_names.hpp"
#include "../loader/settings_check.hpp"

#include <cstdio>
#include <cstdlib>

namespace
{
    int g_failures{};

    void Check(bool condition, const char* what)
    {
        if (!condition)
        {
            std::fprintf(stderr, "FAIL: %s\n", what);
            ++g_failures;
        }
    }

    using loader::keys::Binding;
    using loader::keys::ParseBinding;

    void KeyGrammar()
    {
        Binding binding{};
        Check(ParseBinding("F12", binding) && binding.key == loader::keys::kF1 + 11 && !binding.control, "F12");
        Check(ParseBinding("f1", binding) && binding.key == loader::keys::kF1, "f1 lower case");
        Check(ParseBinding("ctrl+shift+r", binding) && binding.key == 'R' && binding.control && binding.shift && !binding.alt, "ctrl+shift+r");
        Check(ParseBinding("ALT + Numpad5", binding) && binding.key == loader::keys::kNumpad0 + 5 && binding.alt, "alt+numpad5 with spaces");
        Check(ParseBinding("0x7b", binding) && binding.key == 0x7B, "hex code");
        Check(ParseBinding("pageup", binding) && binding.key == 0x21, "named key");
        Check(ParseBinding("caret", binding) && binding.key == 0xDC, "caret (German ^)");
        Check(ParseBinding("", binding) && !binding.Enabled(), "empty = unbound");
        Check(ParseBinding("none", binding) && !binding.Enabled(), "none = unbound");
        Check(!ParseBinding("f25", binding), "f25 rejected");
        Check(!ParseBinding("ctrl+", binding), "modifier only rejected");
        Check(!ParseBinding("banana", binding), "unknown name rejected");
        Check(!ParseBinding("0x100", binding), "hex out of range rejected");
        Binding described{};
        ParseBinding("ctrl+alt+f11", described);
        Check(loader::keys::Describe(described) == "ctrl+alt+f11", "describe round trip");
        ParseBinding("shift+numpad9", described);
        Check(loader::keys::Describe(described) == "shift+numpad9", "describe numpad");
        Check(loader::keys::Describe(Binding{}) == "none", "describe unbound");
    }

    DayzSettingDesc Desc(DayzSettingType type, const char* key, const char* fallback,
        double minimum = 0.0, double maximum = 0.0, const char* choices = nullptr)
    {
        DayzSettingDesc desc{};
        desc.struct_size = sizeof(desc);
        desc.key = key;
        desc.type = type;
        desc.default_value = fallback;
        desc.minimum = minimum;
        desc.maximum = maximum;
        desc.choices = choices;
        return desc;
    }

    void SettingValidation()
    {
        using namespace loader::settings_check;
        const DayzSettingDesc boolean = Desc(DAYZ_SETTING_BOOL, "stereo.enabled", "true");
        Check(ValidDescriptor(boolean), "bool descriptor");
        Check(Accepts(boolean, "false") && Accepts(boolean, "1") && Accepts(boolean, "On"), "bool values");
        Check(!Accepts(boolean, "maybe"), "bool rejects text");

        const DayzSettingDesc number = Desc(DAYZ_SETTING_FLOAT, "stereo.camera_separation", "-0.064", -1.0, 1.0);
        Check(ValidDescriptor(number), "float descriptor");
        Check(Accepts(number, "0.5") && Accepts(number, "-1") && Accepts(number, "1.0"), "float in range");
        Check(!Accepts(number, "1.5") && !Accepts(number, "abc") && !Accepts(number, ""), "float out of range / text / empty");

        const DayzSettingDesc integer = Desc(DAYZ_SETTING_INT, "stereo.frame_lag", "1", 0.0, 8.0);
        Check(Accepts(integer, "3") && !Accepts(integer, "2.5") && !Accepts(integer, "9"), "int values");

        const DayzSettingDesc unbounded = Desc(DAYZ_SETTING_FLOAT, "a.b", "0");
        Check(Accepts(unbounded, "123456.0"), "min==max means unbounded");

        const DayzSettingDesc choice = Desc(DAYZ_SETTING_ENUM, "stereo.fit_mode", "stretch", 0, 0, "contain|stretch|cover");
        Check(ValidDescriptor(choice), "enum descriptor");
        Check(Accepts(choice, "cover") && !Accepts(choice, "zoom") && !Accepts(choice, "stretc"), "enum values");
        DayzSettingDesc badChoice = choice;
        badChoice.choices = nullptr;
        Check(!ValidDescriptor(badChoice), "enum without choices rejected");

        DayzSettingDesc badDefault = Desc(DAYZ_SETTING_INT, "a.b", "x", 0, 1);
        Check(!ValidDescriptor(badDefault), "default must validate");
        DayzSettingDesc noDot = Desc(DAYZ_SETTING_BOOL, "enabled", "true");
        Check(!ValidDescriptor(noDot), "key needs section.key");
        DayzSettingDesc oldStruct = boolean;
        oldStruct.struct_size = 4;
        Check(!ValidDescriptor(oldStruct), "struct_size checked");

        const DayzSettingDesc text = Desc(DAYZ_SETTING_STRING, "debug.plugin", "dayz_openxr_debug.dll");
        Check(Accepts(text, "anything.dll") && !Accepts(text, "two\nlines"), "string values");

        std::string section, key;
        Check(SplitKey("stereo.camera_separation", section, key) && section == "stereo" && key == "camera_separation", "split key");
        Check(!SplitKey("stereo.", section, key) && !SplitKey(".x", section, key), "split rejects empty halves");
    }
}

int main()
{
    KeyGrammar();
    SettingValidation();
    if (g_failures)
    {
        std::fprintf(stderr, "key_names_test: %d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }
    std::puts("key_names_test: hotkey grammar and setting validation passed");
    return EXIT_SUCCESS;
}
