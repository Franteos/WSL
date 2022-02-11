#include "stdafx.h"
#include "gtest/gtest.h"
#include "splash_controller.h"

namespace Oobe
{
    // Fake strategies to exercise the Splash controller state machine.
    struct NothingWorksStrategy
    {
        static bool do_create_process(const std::filesystem::path& exePath,
                                      STARTUPINFO& startup,
                                      PROCESS_INFORMATION& process)
        {
            return false;
        }

        static HWND do_find_window_by_thread_id(DWORD threadId)
        {
            return nullptr;
        }
        // The other methods will never be called, so there is no need to define them. Otherwise it would not even
        // compile.
    }; // struct NothingWorksStrategy

    struct EverythingWorksStrategy
    {
        static bool do_create_process(const std::filesystem::path& exePath,
                                      STARTUPINFO& startup,
                                      PROCESS_INFORMATION& process)
        {
            return true;
        }

        static HWND do_find_window_by_thread_id(DWORD threadId)
        {
            // no risk because this handle will not be used for anything besides passing around.
            return GetConsoleWindow();
        }
        static bool do_show_window(HWND window)
        {
            return true;
        }
        static bool do_hide_window(HWND window)
        {
            return true;
        }
        static bool do_place_behind(HWND toBeFront, HWND toBeBehind)
        {
            return true;
        }
        static void do_forcebly_close(HWND window)
        { }

        static void do_gracefully_close(HWND window)
        { }
        // The other methods will never be called, so there is no need to define them. Otherwise it would not even
        // compile.
    }; // struct EverythingWorksStrategy

    // The whole purpose of adding this state machine technique was to improve overall testability.
    TEST(SplashControllerTests, LaunchFailedShouldStayIdle)
    {
        using Controller = SplashController<NothingWorksStrategy>;
        Controller controller{"./do_not_exists", GetStdHandle(STD_OUTPUT_HANDLE)};
        controller.sm.addEvent(Controller::Events::Run{&controller}); // This fails but it is a valid transition.
        ASSERT_TRUE(controller.sm.isCurrentStateA<Controller::States::Idle>());
    }

    TEST(SplashControllerTests, FailedToFindWindowShouldStayIdle)
    {
        struct CantFindWindowStrategy
        {
            static bool do_create_process(const std::filesystem::path& exePath,
                                          STARTUPINFO& startup,
                                          PROCESS_INFORMATION& process)
            {
                return true;
            }

            static HWND do_find_window_by_thread_id(DWORD threadId)
            {
                return nullptr;
            }
            // When we attempt to push the Close event, compiler detects the need for the two methods below.
            static void do_forcebly_close(HWND window)
            { }

            static void do_gracefully_close(HWND window)
            { }
        }; // struct SplashStrategy
        using Controller = SplashController<CantFindWindowStrategy>;
        Controller controller{"cmd.exe", GetStdHandle(STD_OUTPUT_HANDLE)};
        controller.sm.addEvent(Controller::Events::Run{&controller});
        ASSERT_TRUE(controller.sm.isCurrentStateA<Controller::States::Idle>());
        controller.sm.addEvent(Controller::Events::Close{});
        ASSERT_TRUE(controller.sm.isCurrentStateA<Controller::States::Idle>());
    }

    TEST(SplashControllerTests, AHappySequenceOfEvents)
    {
        using Controller = SplashController<EverythingWorksStrategy>;
        Controller controller{"./do_not_exists", GetStdHandle(STD_OUTPUT_HANDLE)};
        auto transition = controller.sm.addEvent(Controller::Events::Run{&controller});
        // Since everything works in this realm, all transitions below should be valid...
        ASSERT_TRUE(transition.has_value());
        ASSERT_TRUE(controller.sm.isCurrentStateA<Controller::States::Visible>());
        transition = controller.sm.addEvent(Controller::Events::ToggleVisibility{});
        ASSERT_TRUE(transition.has_value());
        ASSERT_TRUE(controller.sm.isCurrentStateA<Controller::States::Hidden>());
        transition = controller.sm.addEvent(Controller::Events::ToggleVisibility{});
        ASSERT_TRUE(transition.has_value());
        ASSERT_TRUE(controller.sm.isCurrentStateA<Controller::States::Visible>());
        transition = controller.sm.addEvent(Controller::Events::ToggleVisibility{});
        ASSERT_TRUE(transition.has_value());
        ASSERT_TRUE(controller.sm.isCurrentStateA<Controller::States::Hidden>());
        transition = controller.sm.addEvent(Controller::Events::PlaceBehind{GetConsoleWindow()});
        ASSERT_TRUE(transition.has_value());
        ASSERT_TRUE(controller.sm.isCurrentStateA<Controller::States::Visible>());
        transition = controller.sm.addEvent(Controller::Events::Close{});
        ASSERT_TRUE(transition.has_value());
        ASSERT_TRUE(controller.sm.isCurrentStateA<Controller::States::ShouldBeClosed>());
    }
    // This proves to be impossible to run the splash application more than once after the first success.
    TEST(SplashControllerTests, OnlyIdleStateAcceptsRunEvent)
    {
        using Controller = SplashController<EverythingWorksStrategy>;
        Controller controller{"./do_not_exists", GetStdHandle(STD_OUTPUT_HANDLE)};
        auto transition = controller.sm.addEvent(Controller::Events::Run{&controller});
        // Since everything works in this realm, all transitions below should be valid...
        ASSERT_TRUE(transition.has_value());
        ASSERT_TRUE(controller.sm.isCurrentStateA<Controller::States::Visible>());

        // now comes the interesting part: all other states should refuse the Run event

        // Exercising the Visble State.
        transition = controller.sm.addEvent(Controller::Events::Run{&controller});
        ASSERT_FALSE(transition.has_value());
        // state should remain the previous one.
        ASSERT_TRUE(controller.sm.isCurrentStateA<Controller::States::Visible>());

        transition = controller.sm.addEvent(Controller::Events::ToggleVisibility{});
        ASSERT_TRUE(transition.has_value());
        ASSERT_TRUE(controller.sm.isCurrentStateA<Controller::States::Hidden>());

        // Exercising the Hidden State.
        transition = controller.sm.addEvent(Controller::Events::Run{&controller});
        ASSERT_FALSE(transition.has_value());
        // state should remain the previous one.
        ASSERT_TRUE(controller.sm.isCurrentStateA<Controller::States::Hidden>());

        transition = controller.sm.addEvent(Controller::Events::Close{});
        ASSERT_TRUE(transition.has_value());
        ASSERT_TRUE(controller.sm.isCurrentStateA<Controller::States::ShouldBeClosed>());
        // Exercising the ShouldBeClosed State. Not even this once accepts re-running the splash. Should we?
        transition = controller.sm.addEvent(Controller::Events::Run{&controller});
        ASSERT_FALSE(transition.has_value());
        // state should remain the previous one.
        ASSERT_TRUE(controller.sm.isCurrentStateA<Controller::States::ShouldBeClosed>());
    }
    // This proves to be impossible to close the window twice.
    TEST(SplashControllerTests, MustCloseOnlyOnce)
    {
        // Remember that in this realm everything just works...
        using Controller = SplashController<EverythingWorksStrategy>;
        Controller controller{"./do_not_exists", GetStdHandle(STD_OUTPUT_HANDLE)};
        auto transition = controller.sm.addEvent(Controller::Events::Run{&controller});
        ASSERT_TRUE(transition.has_value());
        ASSERT_TRUE(controller.sm.isCurrentStateA<Controller::States::Visible>());
        transition = controller.sm.addEvent(Controller::Events::Close{});
        ASSERT_TRUE(transition.has_value());
        ASSERT_TRUE(controller.sm.isCurrentStateA<Controller::States::ShouldBeClosed>());
        // silly attempts start here.
        transition = controller.sm.addEvent(Controller::Events::ToggleVisibility{});
        ASSERT_FALSE(transition.has_value());
        ASSERT_TRUE(controller.sm.isCurrentStateA<Controller::States::ShouldBeClosed>());
        transition = controller.sm.addEvent(Controller::Events::Close{});
        ASSERT_FALSE(transition.has_value()); // if closing twice worked, this assertion would fail.
        ASSERT_TRUE(controller.sm.isCurrentStateA<Controller::States::ShouldBeClosed>());

        // since it's not possible to run a second time...
        transition = controller.sm.addEvent(Controller::Events::Run{&controller});
        ASSERT_FALSE(transition.has_value());
        ASSERT_TRUE(controller.sm.isCurrentStateA<Controller::States::ShouldBeClosed>());

        // and here we make sure the state machine was not fooled by the attempt to run.
        transition = controller.sm.addEvent(Controller::Events::Close{});
        ASSERT_FALSE(transition.has_value());
        ASSERT_TRUE(controller.sm.isCurrentStateA<Controller::States::ShouldBeClosed>());
    }
}