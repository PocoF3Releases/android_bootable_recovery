# Recovery ADB checks

Enable ADB requests init through `sys.usb.config.recovery_adb`, using the
existing `usb_prop` namespace. Init sets `ro.adb.secure.recovery=0` and
restarts adbd. Recovery checks acknowledgement and the authentication
property before reporting success or hiding the action; failure remains
retryable. This explicit recovery action adds no SELinux permissions and
does not enable Android root ADB.

Previously recovery attempted restricted read-only property creation and
service control directly, ignored failures, and incorrectly reported success.

Run from the recovery source root:

```sh
python3 tests/test_enable_adb.py
```

The three host tests cover success and failure, the init action and existing
policy, and compilation of the ADB menu action against checkout headers.
Live recovery ADB was observed connected with request state `enabled` and
authentication property `0` on the user/Enforcing build.
