#!/usr/bin/env bash
set -euo pipefail

task_root="$(cd -- "$(dirname -- "$0")/.." && pwd)"
cd "$task_root"

if [[ -z "${JAVA_HOME:-}" ]]; then
    for candidate in /usr/lib64/jvm/java-17-openjdk-17 /usr/lib64/jvm/java-17-openjdk /usr/lib/jvm/java-17-openjdk-amd64; do
        if [[ -x "$candidate/bin/javac" ]]; then
            export JAVA_HOME="$candidate"
            break
        fi
    done
fi
if [[ -z "${JAVA_HOME:-}" || ! -x "$JAVA_HOME/bin/javac" ]]; then
    echo 'Install JDK 17 and set JAVA_HOME to its directory.' >&2
    exit 1
fi

export GRADLE_USER_HOME="${GRADLE_USER_HOME:-$task_root/.build-support/gradle}"
export ANDROID_USER_HOME="${ANDROID_USER_HOME:-$task_root/.build-support/android-user}"
if [[ $# -eq 0 ]]; then
    set -- assembleDebug lintDebug testDebugUnitTest
fi
exec ./gradlew --no-daemon "$@"
