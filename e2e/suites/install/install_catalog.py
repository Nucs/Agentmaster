"""The install lane's scenarios: which test proves which entries of flows/19's catalog, and what machine each needs.

PURE DATA, no imports: the host's `e2e/host/install_lane.py` reads this file on the PC (where neither pytest nor the
driver is installed) to plan the run — one freshly reset desktop per scenario — and the guest's
`suites/install/conftest.py` checks at collection that every install test is listed here, so a new test cannot run
without a plan (and its catalog ids) or slip into a run that did not reset for it.

Each scenario:
    test     the test's node id below suites/install/ ("<file>::<function>")
    ids      the catalog entries it proves (doc/agentmaster/flows/19-install-update-profiles.md, "Every install
             scenario"); keep the two lists in step
    flow     the INS flow
    machine  what the lane must prepare beyond a fresh desktop (the catalog's "Machine" tags; the host skips a
             scenario that asks for one PREPARED does not hold, and says so)
    minutes  a rough duration in the guest, without the reset and the provisioning (the host prints the plan with it)
"""

SCENARIOS = [
    {"test": "test_install_script.py::test_just_for_you_life_cycle",
     "ids": ["IS-17", "IS-01", "IS-02", "IS-03", "IS-04"], "flow": "INS-01", "machine": [], "minutes": 2},
    {"test": "test_install_script.py::test_for_all_users_life_cycle",
     "ids": ["IS-18", "IS-19", "IS-20"], "flow": "INS-03", "machine": [], "minutes": 2},
    {"test": "test_install_script.py::test_the_callers_powershell_keeps_nothing",
     "ids": ["IS-10"], "flow": "INS-01", "machine": [], "minutes": 2},
    {"test": "test_install_script.py::test_install_dir_rules",
     "ids": ["IS-14", "IS-15", "IS-16"], "flow": "INS-01", "machine": [], "minutes": 2},
    {"test": "test_install_typed.py::test_the_installer_answered_in_console_windows",
     "ids": ["IS-05", "IS-06"], "flow": "INS-01", "machine": [], "minutes": 3},
    {"test": "test_install_typed.py::test_the_installer_typed_into_a_tab_of_the_copy_it_changes",
     "ids": ["IS-07", "IS-08", "IS-09"], "flow": "INS-01", "machine": [], "minutes": 5},
    {"test": "test_install_typed.py::test_the_all_users_upgrade_typed_into_a_tab",
     "ids": ["IS-25"], "flow": "INS-03", "machine": [], "minutes": 3},
    {"test": "test_install_typed.py::test_ctrl_c_half_way_through_an_upgrade",
     "ids": ["IS-13"], "flow": "INS-01", "machine": [], "minutes": 2},
    {"test": "test_install_update.py::test_a_refused_token_is_dropped",
     "ids": ["IS-11"], "flow": "INS-01", "machine": ["net"], "minutes": 2},
    {"test": "test_install_update.py::test_irm_iex_with_a_current_install",
     "ids": ["IS-12"], "flow": "INS-01", "machine": ["net"], "minutes": 2},
    {"test": "test_install_update.py::test_the_startup_prompt_and_its_choices",
     "ids": ["IS-81", "IS-82"], "flow": "INS-06", "machine": ["net"], "minutes": 6},
    {"test": "test_install_update.py::test_update_now_from_the_cog",
     "ids": ["IS-80"], "flow": "INS-06", "machine": ["net"], "minutes": 3},
    {"test": "test_install_update.py::test_a_failed_update_starts_the_old_version_again",
     "ids": ["IS-87"], "flow": "INS-06", "machine": ["net"], "minutes": 3},
    {"test": "test_install_first_launch.py::test_install_just_for_you",
     "ids": ["IS-40", "IS-41", "IS-42", "IS-99", "IS-88", "IS-104"], "flow": "INS-02", "machine": [], "minutes": 2},
    {"test": "test_install_first_launch.py::test_ask_me_later_then_keep_it_portable",
     "ids": ["IS-43", "IS-44"], "flow": "INS-02", "machine": [], "minutes": 2},
    {"test": "test_install_first_launch.py::test_a_newer_copy_updates_and_an_older_one_hands_over",
     "ids": ["IS-45", "IS-46", "IS-105"], "flow": "INS-02", "machine": [], "minutes": 3},
    {"test": "test_install_first_launch.py::test_install_for_all_users",
     "ids": ["IS-47"], "flow": "INS-03", "machine": [], "minutes": 2},
    {"test": "test_install_first_launch.py::test_both_installs_current",
     "ids": ["IS-49"], "flow": "INS-02", "machine": [], "minutes": 2},
    {"test": "test_install_first_launch.py::test_install_here",
     "ids": ["IS-51"], "flow": "INS-02", "machine": [], "minutes": 2},
    {"test": "test_install_first_launch.py::test_install_to_a_folder_you_pick",
     "ids": ["IS-50"], "flow": "INS-02", "machine": [], "minutes": 2},
    {"test": "test_install_first_launch.py::test_the_old_scripts_copy_is_updated",
     "ids": ["IS-57"], "flow": "INS-02", "machine": ["old"], "minutes": 3},
    {"test": "test_install_coexistence.py::test_a_users_own_copy_takes_over",
     "ids": ["IS-71", "IS-72", "IS-73", "IS-74", "IS-75"], "flow": "INS-05", "machine": [], "minutes": 3},
    {"test": "test_install_coexistence.py::test_the_shared_copys_upkeep",
     "ids": ["IS-62", "IS-76"], "flow": "INS-05", "machine": [], "minutes": 3},
    {"test": "test_install_coexistence.py::test_uninstall_from_apps_and_features",
     "ids": ["IS-91", "IS-92", "IS-93"], "flow": "INS-07", "machine": [], "minutes": 2},
    {"test": "test_install_coexistence.py::test_uninstall_from_the_cog",
     "ids": ["IS-90"], "flow": "INS-07", "machine": [], "minutes": 3},
    {"test": "test_install_coexistence.py::test_the_old_name_forwards",
     "ids": ["IS-97", "IS-98"], "flow": "INS-09", "machine": [], "minutes": 2},
    {"test": "test_install_accounts.py::test_another_account_runs_the_installer",
     "ids": ["IS-26", "IS-27"], "flow": "INS-01", "machine": ["std"], "minutes": 3},
    {"test": "test_install_accounts.py::test_a_standard_user_and_the_shared_copy",
     "ids": ["IS-63", "IS-64", "IS-65"], "flow": "INS-03", "machine": ["std"], "minutes": 4},
    {"test": "test_install_accounts.py::test_another_account_sees_the_question",
     "ids": ["IS-53"], "flow": "INS-02", "machine": ["std"], "minutes": 2},
    {"test": "test_install_accounts.py::test_another_administrator_sets_it_up_for_the_signed_in_user",
     "ids": ["IS-52"], "flow": "INS-04", "machine": ["adm2"], "minutes": 3},
    {"test": "test_install_accounts.py::test_another_administrator_runs_the_installer",
     "ids": ["IS-29"], "flow": "INS-04", "machine": ["adm2"], "minutes": 3},
    {"test": "test_install_old_releases.py::test_an_older_release_for_all_users",
     "ids": ["IS-21", "IS-22", "IS-23"], "flow": "INS-03", "machine": ["old"], "minutes": 4},
    {"test": "test_install_old_releases.py::test_a_release_without_a_manifest",
     "ids": ["IS-24", "IS-95"], "flow": "INS-07", "machine": ["old"], "minutes": 3},
    {"test": "test_install_system.py::test_system_installs_for_the_signed_in_user",
     "ids": ["IS-28"], "flow": "INS-04", "machine": ["sys"], "minutes": 3},
    {"test": "test_install_system.py::test_a_copy_started_in_session_0",
     "ids": ["IS-54"], "flow": "INS-02", "machine": ["sys"], "minutes": 2},
    {"test": "test_install_keeps_data.py::test_just_for_you_beside_the_all_users_copy_keeps_the_profile",
     "ids": ["IS-100", "IS-101"], "flow": "INS-02", "machine": [], "minutes": 4},
    {"test": "test_install_keeps_data.py::test_just_for_you_while_the_all_users_copy_runs_keeps_the_profile",
     "ids": ["IS-102"], "flow": "INS-02", "machine": [], "minutes": 4},
    {"test": "test_install_keeps_data.py::test_just_for_you_over_the_0_7_0_package_keeps_the_profile",
     "ids": ["IS-103"], "flow": "INS-08", "machine": ["pkg"], "minutes": 5},
    {"test": "test_install_keeps_data.py::test_a_bin_folder_in_the_way_is_kept_aside",
     "ids": ["IS-106"], "flow": "INS-02", "machine": [], "minutes": 2},
    {"test": "test_install_keeps_data.py::test_the_installer_keeps_a_bin_folder_in_the_way",
     "ids": ["IS-107"], "flow": "INS-01", "machine": [], "minutes": 2},
]
"""Every automated install scenario, in the order the lane runs them."""

PREPARED = frozenset({"std", "adm2", "old", "sys", "net", "pkg"})
"""The Machine tags the lane can prepare today (besides "fresh"): "std" and "adm2" (provision.ps1 -Accounts makes the
standard account qastd and the administrator qaadmin), "old" (provision.ps1 -Old extracts v0.7.10 as published),
"sys" (nothing to provision: amdriver.install.run_as_system makes a one-shot scheduled task as SYSTEM) and "pkg" (the
host stages the v0.7.0 release's msixbundle and certificate into the kit: install_lane.py stage_package_kit; the test
trusts the certificate and adds the bundle itself). UAC, the policy, a demotion, the network and a sign-in are the
catalog's "planned" column."""
