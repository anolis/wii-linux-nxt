#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0

set -euo pipefail

usage()
{
	printf 'Usage: %s [options]\n\n' "${0##*/}"
	cat <<'EOF'
Upload and run the GCN render-UAPI hardware test over SSH.

Options:
  --host HOST          Wii address (default: WII_SSH_HOST or 10.3.10.12)
  --module FILE        gcn-gx module (default: drivers/video/fbdev/gcn-gx.ko)
  --client FILE        test client (default: /media/anolis/dev/wii-gcn-scratch/wii-gcn-render-test)
  --client-args ARGS   arguments passed to the test client
  --kms-client FILE    optional linear-render/KMS presentation client
  --kms-hold SECONDS   presentation duration (default: 5)
  --kms-mode MODE      KMS scene: pattern or triangle (default: pattern)
  --flip-client FILE   optional linear-render/KMS page-flip client
  --flip-count COUNT   page flips requested from flip client (default: 120)
  --flip-format FORMAT source format: rgb565, xrgb8888, or
                       xrgb8888-native[-tiled] (default: rgb565)
  --module-args ARGS   arguments passed to insmod
  --reuse-remote       require checksum-matched files already in /tmp
  --expect-param N=V  Verify a loaded module parameter before testing
  --quiet-kernel      Keep kernel diagnostics in dmesg, suppress tty spam
  --keep-loaded        leave gcn_gx loaded after a successful test
  --allow-dirty        permit testing from an uncommitted source tree

The Wii console reports connection, transfer, load, test, unload, and final
status. EFB iteration tests show a progress bar on both consoles; detailed
kernel diagnostics remain in dmesg. The original console log level is restored
on exit. Other test clients retain their normal terminal output.
EOF
}

ssh_host=${WII_SSH_HOST:-10.3.10.12}
module=
client=/media/anolis/dev/wii-gcn-scratch/wii-gcn-render-test
client_args=
kms_client=
kms_hold=5
kms_mode=pattern
flip_client=
flip_count=120
flip_format=rgb565
module_args=
reuse_remote=0
keep_loaded=0
quiet_kernel=0
expect_param=
allow_dirty=0

while (($#)); do
	case "$1" in
	--expect-param)
		expect_param=$2
		shift
		;;
	--quiet-kernel)
		quiet_kernel=1
		;;
	--host)
		ssh_host=$2
		shift
		;;
	--module)
		module=$2
		shift
		;;
	--client)
		client=$2
		shift
		;;
	--client-args)
		client_args=$2
		shift
		;;
	--kms-client)
		kms_client=$2
		shift
		;;
	--kms-hold)
		kms_hold=$2
		shift
		;;
	--kms-mode)
		kms_mode=$2
		shift
		;;
	--flip-client)
		flip_client=$2
		shift
		;;
	--flip-count)
		flip_count=$2
		shift
		;;
	--flip-format)
		flip_format=$2
		shift
		;;
	--module-args)
		module_args=$2
		shift
		;;
	--reuse-remote)
		reuse_remote=1
		;;
	--keep-loaded)
		keep_loaded=1
		;;
	--allow-dirty)
		allow_dirty=1
		;;
	-h|--help)
		usage
		exit 0
		;;
	*)
		printf 'Unknown argument: %s\n' "$1" >&2
		usage >&2
		exit 2
		;;
	esac
	shift
done

repo=$(git rev-parse --show-toplevel)
cd "$repo"
module=${module:-$repo/drivers/video/fbdev/gcn-gx.ko}
[[ -f $module ]] || { printf 'Module not found: %s\n' "$module" >&2; exit 1; }
[[ -f $client ]] || { printf 'Client not found: %s\n' "$client" >&2; exit 1; }
if [[ -n $kms_client && ! -f $kms_client ]]; then
	printf 'KMS client not found: %s\n' "$kms_client" >&2
	exit 1
fi
if [[ -n $flip_client && ! -f $flip_client ]]; then
	printf 'Page-flip client not found: %s\n' "$flip_client" >&2
	exit 1
fi
if [[ ! $kms_hold =~ ^[0-9]+$ ]]; then
	printf 'Invalid KMS hold duration: %s\n' "$kms_hold" >&2
	exit 2
fi
if [[ $kms_mode != pattern && $kms_mode != triangle ]]; then
	printf 'Invalid KMS mode: %s\n' "$kms_mode" >&2
	exit 2
fi
if [[ ! $flip_count =~ ^[1-9][0-9]*$ ]] || (( flip_count > 10000 )); then
	printf 'Invalid page-flip count: %s\n' "$flip_count" >&2
	exit 2
fi
if [[ $flip_format != rgb565 && $flip_format != xrgb8888 &&
      $flip_format != xrgb8888-native &&
      $flip_format != xrgb8888-native-tiled ]]; then
	printf 'Invalid page-flip source format: %s\n' "$flip_format" >&2
	exit 2
fi
if (( ! allow_dirty )) &&
   [[ -n $(git status --porcelain --untracked-files=normal) ]]; then
	printf 'Refusing a dirty tree; commit first or pass --allow-dirty.\n' >&2
	exit 1
fi
if [[ ! $module_args =~ ^[A-Za-z0-9_.,=+\ -]*$ ]]; then
	printf 'Unsafe character in module arguments: %s\n' "$module_args" >&2
	exit 2
fi
if [[ ! $client_args =~ ^[A-Za-z0-9_./,=+\ -]*$ ]]; then
	printf 'Unsafe character in client arguments: %s\n' "$client_args" >&2
	exit 2
fi

if [[ -n $expect_param && ! $expect_param =~ ^[A-Za-z0-9_]+=[A-Za-z0-9_]+$ ]]; then
	printf 'Invalid expected parameter: %s\n' "$expect_param" >&2
	exit 2
fi

ssh_key=${WII_SSH_KEY:-$HOME/.ssh/id_rsa}
if [[ $ssh_host == *@* ]]; then
	remote=$ssh_host
else
	remote=root@$ssh_host
fi
mkdir -p /media/anolis/dev/wii-gcn-scratch
export TMPDIR=/media/anolis/dev/wii-gcn-scratch
ssh_control_path=/media/anolis/dev/wii-gcn-scratch/wii-gcn-render-ssh-${UID}-$$
ssh_options=(
	-i "$ssh_key"
	-o IdentitiesOnly=yes
	-o BatchMode=yes
	-o PubkeyAcceptedAlgorithms=+ssh-rsa
	-o StrictHostKeyChecking=no
	-o UserKnownHostsFile=/dev/null
	-o ForwardX11=no
	-o RequestTTY=no
	-o ConnectTimeout=8
	-o LogLevel=ERROR
	-o ControlMaster=auto
	-o ControlPersist=30
	-o ControlPath="$ssh_control_path"
	-o ServerAliveInterval=5
	-o ServerAliveCountMax=3
)

remote_exec()
{
	# shellcheck disable=SC2029
	ssh "${ssh_options[@]}" "$remote" "$1"
}

remote_notice()
{
	remote_exec "
		printf '<6>gcn-render-cycle: %s\\n' '$1' > /dev/kmsg
		printf '\\n=== GCN RENDER: %s ===\\n' '$1' > /dev/tty0 2>/dev/null || true
	"
}

close_ssh_master()
{
	ssh "${ssh_options[@]}" -O exit "$remote" >/dev/null 2>&1 || true
}

loaded=0
saved_console_level=
cleanup()
{
	local status=$?

	if (( loaded && (! keep_loaded || status != 0) )); then
		remote_notice "unloading accelerator module" >/dev/null 2>&1 || true
		remote_exec "rmmod gcn_gx" >/dev/null 2>&1 || true
		loaded=0
		remote_notice "CPU console restored" >/dev/null 2>&1 || true
	fi
	if [[ -n $saved_console_level ]]; then
		remote_exec "printf '%s\\n' '$saved_console_level' > /proc/sys/kernel/printk" >/dev/null 2>&1 || true
	fi
	close_ssh_master
}
trap cleanup EXIT

upload_verified()
{
	local source=$1 destination=$2 label=$3
	local local_sha remote_sha

	local_sha=$(sha256sum "$source" | awk '{print $1}')
	if (( reuse_remote )); then
		remote_sha=$(remote_exec "sha256sum $destination 2>/dev/null | cut -d' ' -f1")
		[[ $remote_sha == "$local_sha" ]] || {
			printf '%s checksum mismatch: local=%s remote=%s\n' \
				"$label" "$local_sha" "${remote_sha:-missing}" >&2
			return 1
		}
	else
		remote_notice "downloading $label"
		remote_exec "cat > $destination.new" < "$source"
		remote_sha=$(remote_exec "sha256sum $destination.new | cut -d' ' -f1")
		if [[ $remote_sha != "$local_sha" ]]; then
			remote_exec "rm -f $destination.new"
			printf '%s checksum mismatch: local=%s remote=%s\n' \
				"$label" "$local_sha" "${remote_sha:-missing}" >&2
			return 1
		fi
		remote_exec "chmod 755 $destination.new && mv -f $destination.new $destination"
	fi
	remote_notice "$label verified"
}

printf 'Opening persistent SSH connection to %s\n' "$remote"
ssh "${ssh_options[@]}" -Nf "$remote"
remote_notice "connected"
remote_notice "unloading prior accelerator"
remote_exec "rmmod gcn_gx 2>/dev/null || true"

remote_module=/tmp/gcn-gx.ko
remote_client=/tmp/wii-gcn-render-test
remote_kms_client=/tmp/wii-gcn-kms-render-test
remote_flip_client=/tmp/wii-gcn-kms-flip-test
upload_verified "$module" "$remote_module" module
upload_verified "$client" "$remote_client" "test client"
if [[ -n $kms_client ]]; then
	upload_verified "$kms_client" "$remote_kms_client" "KMS test client"
fi
if [[ -n $flip_client ]]; then
	upload_verified "$flip_client" "$remote_flip_client" "page-flip test client"
fi

if (( quiet_kernel )); then
	saved_console_level=$(remote_exec "cut -f1 /proc/sys/kernel/printk")
	[[ $saved_console_level =~ ^[0-9]+$ ]] || exit 1
	remote_exec "printf '4\\n' > /proc/sys/kernel/printk"
fi
remote_notice "loading accelerator module"
if [[ " $module_args " =~ [[:space:]]efb_clear_iterations=([1-9][0-9]*)[[:space:]] ]]; then
	efb_iterations=${BASH_REMATCH[1]}
	# Upload the small helper even when reusing the module/client binaries.
	saved_reuse_remote=$reuse_remote
	reuse_remote=0
	upload_verified tools/wii-gcn-efb-progress.sh /tmp/wii-gcn-efb-progress "progress helper"
	reuse_remote=$saved_reuse_remote
	if [[ -z $saved_console_level ]]; then
		saved_console_level=$(remote_exec "cut -f1 /proc/sys/kernel/printk")
	fi
	[[ $saved_console_level =~ ^[0-9]+$ ]] || exit 1
	# Keep every diagnostic record in dmesg, but show only errors on the tty.
	remote_exec "printf '4\\n' > /proc/sys/kernel/printk"
	loaded=1
	remote_exec "/tmp/wii-gcn-efb-progress $remote_module $efb_iterations $module_args"
else
	remote_exec "insmod $remote_module $module_args"
	loaded=1
fi
if [[ -n $expect_param ]]; then
	param_name=${expect_param%%=*}
	param_value=${expect_param#*=}
	actual_value=$(remote_exec "cat /sys/module/gcn_gx/parameters/$param_name")
	[[ $actual_value == "$param_value" ]] || {
		printf 'Module parameter mismatch: %s expected=%s actual=%s\n' \
			"$param_name" "$param_value" "$actual_value" >&2
		exit 1
	}
	remote_notice "parameter $param_name verified $actual_value"
fi
remote_notice "running hardware test"
set +e
remote_exec "$remote_client $client_args"
test_status=$?
set -e

if (( test_status )); then
	remote_notice "TEST FAILED status $test_status"
	exit "$test_status"
fi
if [[ -n $kms_client ]]; then
	remote_notice "running KMS presentation test"
	set +e
	remote_exec "$remote_kms_client /dev/dri/card0 $kms_hold $kms_mode"
	test_status=$?
	set -e
	if (( test_status )); then
		remote_notice "KMS TEST FAILED status $test_status"
		exit "$test_status"
	fi
	remote_notice "KMS presentation restored console"
fi
if [[ -n $flip_client ]]; then
	remote_notice "running $flip_format sustained KMS page-flip test"
	set +e
	remote_exec "$remote_flip_client /dev/dri/card0 $flip_count $flip_format"
	test_status=$?
	set -e
	if (( test_status )); then
		remote_notice "PAGE-FLIP TEST FAILED status $test_status"
		exit "$test_status"
	fi
	remote_notice "KMS page-flip test restored console"
fi
remote_notice "TEST PASSED"
if (( keep_loaded )); then
	remote_notice "accelerator left loaded"
	loaded=0
fi
