#pragma once
#include <filesystem>
#include <variant>
#include <type_traits>

#include "state_machine.h"

namespace Oobe
{
    /// Following the same principles of the SplashController, this implements the states, events and control of the
    /// Ubuntu Desktop Installer OOBE on WSL. Most of the actual work that requires interacting with the operating
    /// system or with the WSL API has been moved out to a strategy class, which by default is InstallerStrategy class.
    ///
    /// InstallerController admits the following states:
    ///
    /// Closed                  - not running, initial state upon startup.
    /// AutoInstalling          - ready to run the OOBE in `autoinstall` mode. This implies text mode with the
    /// assumption
    ///                           that it might be launched in some automated form.
    /// PreparedGui             - preparation steps such as generating the user information used to seed the GUI
    ///                           input fields is complete and detected that OOBE can run in GUI mode.
    /// PreparedTui             - same as above, but TUI mode is required.
    ///                           Both states receive the full command line to invoke the OOBE.
    ///
    /// Ready                   - OOBE was launched asynchronously and it's now ready for interacting with the user.
    ///                           This is useful to let the application do other things while OOBE is preparing its
    ///                           environment and then react when OOBE is ready for the user.
    /// Success                 - OOBE finished successfully. From that point on this controller is meaningless.
    /// UpstreamDefaultInstall  - OOBE cannot run (failed or not existing in this distribution version). From that point
    ///                           on this controller is meaningless.
    ///
    /// TuiReconfig             - Command line parsing requested OOBE reconfiguration variant, but TUI is required.
    /// GuiReconfig             - Same as above, but can run in GUI mode.
    ///
    ///
    /// The expected state transitions are as follows (using PlantUML notation syntax with the <<choice>> notation
    /// removed for brevity):
    ///
    /// [*] --> Closed
    ///
    /// Closed --> Success                  : Events::Reconfig
    /// note on link: Command line detected request for reconfiguration. OOBE is launched synchronously.
    ///
    /// Closed --> AutoInstalling           : Events::AutoInstall{answers_file}
    /// note on link: This must be a direct result of command line option `install --autoinstall <answers_file>`
    /// AutoInstalling --> Success          :  Events::BlockOnInstaller
    ///
    /// Closed --> PreparedTui              : Events::InteractiveInstall
    /// note on link: Upon handling that event, text mode requirement was detected.
    ///
    /// Closed --> PreparedGui              : Events::InteractiveInstall
    /// note on link: As above, but GUI mode is allowed.
    ///
    /// PreparedTui / PreparedGui --> Ready : Events::StartInstaller
    /// note on link
    ///     OOBE was started successfully (Subiquity socket is listening). Application must take any required
    ///     actions to ensure user interactivity at this point (restoring the console for TUI, for example).
    ///     This thread becomes blocked until the OOBE is signaled ready or a defined timeout occurs.
    /// end note
    ///
    /// Ready --> Success                    : Events::BlockOnInstaller
    /// note on link: Receiving the BlockOnInstaller event causes this thread will be blocked until the end of the OOBE.
    ///
    /// NOTE: Any of the states above may transition to UpstreamDefaultInstall if a non-recoverable failure occurs.
    /// The application must ensure there is a path in the program to the original upstream terminal experience.
    struct InstallerPolicy;

    template <typename Policy = InstallerPolicy> class InstallerController
    {
      public:
        enum class Mode
        {
            Gui,
            Text
        };
        struct Events
        {
            // This event is meant to be generated by the command line parsing if the OOBE perform an automatic
            // installation seeded by the [answers_file].
            struct AutoInstall
            {
                std::filesystem::path answers_file;
            };

            // The opposite of the AutoInstalling, triggered by launching in install mode where OOBE exists.
            struct InteractiveInstall
            { };

            // Command line parsing equivalent of `launcher config`. Implies the distro is already installed.
            struct Reconfig
            { };

            // Request for launching the installer asynchronously and report when its ready for user interaction.
            struct StartInstaller
            { };

            // Request for the controller to block this thread waiting the installer to finish its job.
            struct BlockOnInstaller
            { };

            using EventVariant =
              std::variant<InteractiveInstall, AutoInstall, Reconfig, StartInstaller, BlockOnInstaller>;
        };
        struct States
        {
            struct Closed;
            struct AutoInstalling;
            struct PreparedGui;
            struct PreparedTui;
            class Ready;
            struct UpstreamDefaultInstall;
            struct Success;
            using StateVariant =
              std::variant<Closed, AutoInstalling, PreparedGui, PreparedTui, Ready, UpstreamDefaultInstall, Success>;

            // [Definitions]

            struct Closed
            {
                // Prepares the distro and the command line for initiating an auto installation.
                StateVariant on_event(typename Events::AutoInstall event)
                {
                    if (!Policy::is_oobe_available()) {
                        return UpstreamDefaultInstall{E_NOTIMPL};
                    }
                    if (!std::filesystem::exists(event.answers_file)) {
                        wprintf(L"Answers file not found. Cannot proceed with auto installation\n");
                        return UpstreamDefaultInstall{E_FAIL};
                    }
                    auto source{event.answers_file};
                    std::wstring destination{L"/var/tmp/"};
                    destination.append(source.filename().wstring());
                    if (!Policy::copy_file_into_distro(source, destination)) {
                        wprintf(L"Failed to copy the answers file into the distro file system. Cannot proceed with "
                                L"auto installation\n");
                        return UpstreamDefaultInstall{E_FAIL};
                    }

                    std::wstring commandLine{Policy::OobeCommand};
                    commandLine.append(L" --text ");
                    commandLine.append(L"--autoinstall ");
                    commandLine.append(destination);

                    return AutoInstalling{commandLine};
                }

                // Decides whether OOBE must be launched in text or GUI mode and seeds it with user information.
                StateVariant on_event(typename Events::InteractiveInstall /*unused*/)
                {
                    if (!Policy::is_oobe_available()) {
                        return UpstreamDefaultInstall{E_NOTIMPL};
                    }

                    std::wstring commandLine{Policy::OobeCommand};
                    commandLine += Policy::prepare_prefill_info();
                    
                    // OOBE runs GUI by default, unless command line option --text is set.
                    if (Policy::must_run_in_text_mode()) {
                        commandLine.append(L" --text");
                        return PreparedTui{commandLine};
                    }

                    return PreparedGui{commandLine};
                }

                // Effectively launches the OOBE in reconfiguration variant from start to finish.
                StateVariant on_event(typename Events::Reconfig /*unused*/)
                {
                    if (!Policy::is_oobe_available()) {
                        return UpstreamDefaultInstall{E_NOTIMPL};
                    }

                    std::wstring commandLine{Policy::OobeCommand};
                    if (Policy::must_run_in_text_mode()) {
                        commandLine.append(L" --text");
                    }

                    if (auto exitCode = Policy::do_launch_sync(commandLine.c_str()); exitCode != 0) {
                        return UpstreamDefaultInstall{E_FAIL};
                    }
                    return Success{};
                }
            };

            struct AutoInstalling
            {
                std::wstring cli;
                StateVariant on_event(typename Events::BlockOnInstaller /*unused*/)
                {
                    if (auto exitCode = Policy::do_launch_sync(cli.c_str()); exitCode != 0) {
                        return UpstreamDefaultInstall{E_FAIL};
                    }
                    Policy::handle_exit_status();
                    return Success{};
                }
            };

            struct PreparedTui
            {
                std::wstring cli;
                Mode mode = Mode::Text;
                StateVariant on_event(typename Events::StartInstaller /*unused*/)
                {
                    return InstallerController::start_installer_async(mode, cli.c_str());
                }
            };

            struct PreparedGui
            {
                std::wstring cli;
                Mode mode = Mode::Gui;
                StateVariant on_event(typename Events::StartInstaller /*unused*/)
                {
                    return InstallerController::start_installer_async(mode, cli.c_str());
                }
            };

            class Ready
            {
              private:
                HANDLE oobeProcess = nullptr;
                DWORD timeout = 0;

              public:
                Ready(HANDLE oobeProcess, DWORD timeout) : oobeProcess{oobeProcess}, timeout{timeout} {};

                StateVariant on_event(typename Events::BlockOnInstaller /*unused*/)
                {
                    // Policy::consume_process must consume the handle otherwise it will leak.
                    if (auto exitCode = Policy::consume_process(oobeProcess, timeout); exitCode != 0) {
                        return UpstreamDefaultInstall{E_FAIL};
                    }
                    Policy::handle_exit_status();
                    return Success{};
                }
            };

            struct UpstreamDefaultInstall
            {
                HRESULT hr = E_NOTIMPL;
            };

            struct Success
            { };
        };

        // The state machine expects the symbol `State` to be visible.
        // The repetition of `typename` is an unfortunate side effect of having this controller as a template. All event
        // and state types are now dependent and cannot be explicitly referred to without the templated type argument.
        using State = typename States::StateVariant;
        using Event = typename Events::EventVariant;

        internal::state_machine<InstallerController> sm;

      private:
        // Just to avoid code repetition on PreparedGui and PreparedTui states on StartInstaller event.
        // They only differ in the timeout option applied.
        static State start_installer_async(Mode mode, const wchar_t* command)
        {
            DWORD timeoutMs =
              mode == InstallerController::Mode::Text ? INFINITE : static_cast<DWORD>(1000 * 60 * 4); // 4 min.
            HANDLE oobeProcess = Policy::start_installer_async(command);
            if (oobeProcess == nullptr) {
                return InstallerController::States::UpstreamDefaultInstall{E_FAIL};
            }
            return InstallerController::States::Ready{oobeProcess, timeoutMs};
        }
    };
}