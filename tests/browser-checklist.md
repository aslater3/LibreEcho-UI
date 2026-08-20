# Browser smoke-test checklist

- Load the overview with JavaScript enabled and verify the mock/simulated badge.
- Navigate every sidebar item using mouse and keyboard.
- At 375 px width, open the navigation drawer and verify there is no horizontal overflow.
- Change audio, microphone, LED and wake-word controls; verify pending controls disable and failures roll back.
- Scan Wi-Fi, connect to `LibreNet-IoT`, and observe the connecting-to-connected transition.
- Inject a Wi-Fi scan fault and verify the error state is visible.
- Verify reboot, shutdown and factory reset each display a browser confirmation.
- Switch to the Linux backend and verify unavailable hardware panels say “not supported”.
- Verify focus rings are visible in both dark and light themes.
