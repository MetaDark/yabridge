// yabridge: a Wine VST bridge
// Copyright (C) 2020-2021 Robbert van der Helm
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

//! Small helper utilities.

use anyhow::{Context, Result};
use colored::Colorize;
use is_executable::IsExecutable;
use std::collections::hash_map::DefaultHasher;
use std::env;
use std::fs;
use std::hash::Hasher;
use std::os::unix::fs as unix_fs;
use std::os::unix::process::CommandExt;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use textwrap;

use crate::config::{self, Config, KnownConfig, YABRIDGE_HOST_EXE_NAME};
use crate::files::NativeFile;

/// (Part of) the expected output when running `yabridge-host.exe`. Used to verify that everything's
/// working correctly. We'll only match this prefix so we can modify the exact output at a later
/// moment without causing issues.
const YABRIDGE_HOST_EXPECTED_OUTPUT_PREFIX: &str = "Usage: yabridge-";

/// Wrapper around [`std::fs::copy()`](std::fs::copy) with a human readable error message.
pub fn copy<P: AsRef<Path>, Q: AsRef<Path>>(from: P, to: Q) -> Result<u64> {
    fs::copy(&from, &to).with_context(|| {
        format!(
            "Error copying '{}' to '{}'",
            from.as_ref().display(),
            to.as_ref().display()
        )
    })
}

/// Wrapper around [`std::fs::create_dir_all()`](std::fs::create_dir_all) with a human readable
/// error message.
pub fn create_dir_all<P: AsRef<Path>>(path: P) -> Result<()> {
    fs::create_dir_all(&path).with_context(|| {
        format!(
            "Error creating directories for '{}'",
            path.as_ref().display(),
        )
    })
}

/// Wrapper around [`std::fs::remove_dir_all()`](std::fs::remove_dir_all) with a human readable
/// error message.
pub fn remove_dir_all<P: AsRef<Path>>(path: P) -> Result<()> {
    fs::remove_dir_all(&path)
        .with_context(|| format!("Could not remove directory '{}'", path.as_ref().display()))
}

/// Wrapper around [`std::fs::remove_file()`](std::fs::remove_file) with a human readable error
/// message.
pub fn remove_file<P: AsRef<Path>>(path: P) -> Result<()> {
    fs::remove_file(&path)
        .with_context(|| format!("Could not remove '{}'", path.as_ref().display()))
}

/// Wrapper around [`std::os::unix::fs::symlink()`](std::os::unix::fs::symlink) with a human
/// readable error message.
pub fn symlink<P: AsRef<Path>, Q: AsRef<Path>>(src: P, dst: Q) -> Result<()> {
    unix_fs::symlink(&src, &dst).with_context(|| {
        format!(
            "Error symlinking '{}' to '{}'",
            src.as_ref().display(),
            dst.as_ref().display()
        )
    })
}

/// Get the type of a file, if it exists.
pub fn get_file_type(path: PathBuf) -> Option<NativeFile> {
    match path.symlink_metadata() {
        Ok(metadata) if metadata.file_type().is_symlink() => Some(NativeFile::Symlink(path)),
        Ok(metadata) if metadata.file_type().is_dir() => Some(NativeFile::Directory(path)),
        Ok(_) => Some(NativeFile::Regular(path)),
        Err(_) => None,
    }
}

/// Hash the conetnts of a file as an `i64` using Rust's built in hasher. Collisions are not a big
/// issue in our situation so we can get away with this.
///
/// # Note
///
/// We convert the hash to an i64 because the TOML library can't deserialize large u64 values since
/// it uses i64s internally.
pub fn hash_file(file: &Path) -> Result<i64> {
    let mut hasher = DefaultHasher::new();
    hasher.write(
        &fs::read(file)
            .with_context(|| format!("Could not read contents of '{}'", file.display()))?,
    );

    Ok(hasher.finish() as i64)
}

/// Verify that `yabridge-host.exe` can be found when yabridge is run in a host launched from the
/// GUI. We do this by launching a login shell, appending `~/.local/share/yabridge` to the login
/// shell's search path since that's what yabridge also does, and then making the the file can be
/// found. Returns `true` if it can be found, or if we the login shell is set to an unknown shell.
/// In the last case we'll just print a warning since we don't know how to invoke the shell as a
/// login shell. This is needed when using copies to ensure that yabridge can find the host binaries
/// when the VST host is launched from the desktop enviornment.
///
/// This is a bit messy, and with yabridge 2.1 automatically searching in `~/.local/share/yabridge`
/// it's probably not really needed anymore, but it could still be useful in some edge case
/// scenarios.
pub fn verify_path_setup(config: &Config) -> Result<bool> {
    // First we'll check `~/.local/share/yabridge`, since that's a special location where yabridge
    // will always search
    let xdg_data_yabridge_exists = config::yabridge_directories()
        .map(|dirs| {
            dirs.get_data_home()
                .join(YABRIDGE_HOST_EXE_NAME)
                .is_executable()
        })
        .unwrap_or(false);
    if xdg_data_yabridge_exists {
        return Ok(true);
    }

    // Then we'll check the login shell, since DAWs launched from the GUI will have the same
    // environment
    match env::var("SHELL") {
        Ok(shell_path) => {
            // `$SHELL` will often contain a full path, but it doesn't have to
            let shell = Path::new(&shell_path)
                .file_name()
                .and_then(|os_str| os_str.to_str())
                .unwrap_or_else(|| shell_path.as_str());

            // We're using the `-l` flag present in most shells to start a login shell, but some
            // shells don't have this option. According the Bash's man page, another method some
            // shells use to determine that they're being run as a login shell is by checking that
            // `argv[0]` starts with a hyphen, so we'll also do that.
            let mut command = Command::new(&shell_path);
            command.arg0(format!("-{}", &shell_path));

            let command = match shell {
                // All of these shells support the `-l` flag to start a login shell and have a
                // POSIX-compatible `command` builtin
                "ash" | "bash" | "csh" | "ksh" | "dash" | "fish" | "ion" | "sh" | "tcsh"
                | "zsh" => command
                    .arg("-l")
                    .arg("-c")
                    .arg(format!("command -v {}", YABRIDGE_HOST_EXE_NAME)),
                // These shells either have their own implementation of `which` and don't support
                // `command`, or they don't have a seperate login shell flag
                "elvish" | "oil" => command
                    .arg("-c")
                    .arg(format!("command -v {}", YABRIDGE_HOST_EXE_NAME)),
                // xonsh's which implementation is broken as of writing this, so I left it out
                "pwsh" => command
                    .arg("-l")
                    .arg("-c")
                    .arg(format!("which {}", YABRIDGE_HOST_EXE_NAME)),
                "nu" => command
                    .arg("-c")
                    .arg(format!("which {}", YABRIDGE_HOST_EXE_NAME)),
                shell => {
                    eprintln!(
                        "\n{}",
                        wrap(&format!(
                            "WARNING: Yabridgectl does not know how to handle your login shell \
                             '{}', skipping PATH environment variable check. Feel free to open a \
                             feature request in order to get yabridgectl to support your shell.\n\
                             \n\
                             https://github.com/robbert-vdh/yabridge/issues",
                            shell.bright_white(),
                        ))
                    );
                    return Ok(true);
                }
            };

            // For the login shell we want to a clean environment, but we still have to set `$HOME`
            // or else most shells won't know which profile to load
            command
                .env_clear()
                .env("HOME", env::var("HOME").unwrap_or_default());

            match command.stdout(Stdio::null()).stderr(Stdio::null()).status() {
                Ok(status) if status.success() => Ok(true),
                Ok(_) => {
                    eprintln!(
                        "\n{}",
                        wrap(&format!(
                            "Warning: 'yabridge-host.exe' is not present in your login shell's \
                             search path. Yabridge won't be able to run using the copy-based \
                             installation method until this is fixed.\n\
                             Add '{}' to {}'s login shell {} environment variable. See the \
                             troubleshooting section of the readme for more details. Rerun this \
                             command to verify that the variable has been set correctly, and then \
                             reboot your system to complete the setup.\n\
                             \n\
                             https://github.com/robbert-vdh/yabridge#troubleshooting-common-issues",
                            config.files()?.libyabridge_vst2.parent().unwrap().display(),
                            shell.bright_white(),
                            "PATH".bright_white()
                        ))
                    );

                    Ok(false)
                }
                Err(err) => {
                    eprintln!(
                        "\n{}",
                        wrap(&format!(
                            "Warning: could not run {} as a login shell, skipping PATH setup check: \
                             {}",
                            shell.bright_white(), err
                        ))
                    );

                    Ok(true)
                }
            }
        }
        Err(_) => {
            eprintln!("\nWarning: Could not determine login shell, skipping PATH setup check");

            Ok(true)
        }
    }
}

/// Verify that the installed versions of Wine and yabridge will work together properly. This check
/// is only performed once per combination of Wine and yabridge, and we'll update the config with
/// the versions we just tested if the check succeeds. Will return `Err` values if either Wine or
/// `yabridge-host.exe` can't be run.
pub fn verify_wine_setup(config: &mut Config) -> Result<()> {
    // These winelib scripts respect `$WINELOADER`, so we'll do the same thing
    let wine_binary = env::var("WINELOADER").unwrap_or_else(|_| String::from("wine"));
    let wine_version_output = Command::new(&wine_binary)
        .arg("--version")
        .output()
        .with_context(|| {
            format!(
                "Could not run '{}', make sure Wine is installed",
                wine_binary
            )
        })?
        .stdout;
    // Strip the trailing newline just to make the config file a bit neater
    let mut wine_version = String::from_utf8(wine_version_output)?;
    wine_version.pop().unwrap();

    let files = config
        .files()
        .context(format!("Could not find '{}'", YABRIDGE_HOST_EXE_NAME))?;

    // Hash the contents of `yabridge-host.exe.so` since `yabridge-host.exe` is only a Wine
    // generated shell script
    let yabridge_host_hash = hash_file(&files.yabridge_host_exe_so)?;

    // Since these checks can take over a second if wineserver isn't already running we'll only
    // perform them when something has changed
    let current_config = KnownConfig {
        wine_version: wine_version.clone(),
        yabridge_host_hash,
    };
    if config.last_known_config.as_ref() == Some(&current_config) {
        return Ok(());
    }

    let output = Command::new(&files.yabridge_host_exe)
        .output()
        .with_context(|| format!("Could not run '{}'", files.yabridge_host_exe.display()))?;
    let stderr = String::from_utf8(output.stderr)?;

    // There are three scenarios here:
    // - Either everything is fine and we'll see the usage string being printed
    // - Or the used version of Wine is too old and we'll see some line starting with
    //   `002b:err:module:__wine_process_init`
    // - Or the used version of Wine is much newer than what was used to compile yabridge with
    //
    // I don't know if it's possible to differentiate between the second and the third case, so
    // we'll always assume it's Wine that's outdated.
    let mut success = false;
    let mut last_error: Option<&str> = None;
    for line in stderr.lines() {
        if line.starts_with(YABRIDGE_HOST_EXPECTED_OUTPUT_PREFIX) {
            success = true;
            break;
        }

        // Ignore fixme messages here, since those can be produced by wineserver even after the
        // application has errored out
        if line.get(5..10) != Some("fixme") {
            last_error = Some(line);
        }
    }

    if success {
        config.last_known_config = Some(current_config);
        config.write()?;
    } else {
        eprintln!(
            "\n{}",
            wrap(&format!(
                "Warning: Could not run 'yabridge-host.exe'. Wine reported the following error: \n\
                 \n\
                 {}\n\
                 \n\
                 This can happen when using a version of Wine that is much older than the version \
                 that has been used to compile yabridge with. Your current Wine version is '{}'. \
                 See the troubleshooting section of the readme for more information on how to \
                 upgrade your installation of Wine.\n\
                 \n\
                 https://github.com/robbert-vdh/yabridge#troubleshooting-common-issues",
                last_error.unwrap_or("<no_output>").bright_white(),
                wine_version
                    .strip_prefix("wine-")
                    .unwrap_or(&wine_version)
                    .bright_white(),
            ))
        )
    }

    Ok(())
}

/// Wrap a long paragraph of text to terminal width, or 80 characters if the width of the terminal
/// can't be determined. Everything after the first line gets indented with four spaces.
pub fn wrap(text: &str) -> String {
    textwrap::fill(
        text,
        textwrap::Options::new(textwrap::termwidth())
            .splitter(textwrap::NoHyphenation)
            .subsequent_indent("    "),
    )
}
