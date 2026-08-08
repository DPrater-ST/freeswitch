# RingRx FreeSWITCH — `.deb` build pipeline

`.github/workflows/build-freeswitch-deb.yml` builds this fork's FreeSWITCH Debian
packages and publishes them to S3, where the **telephony-iac** VoiceFabric AMI
pipeline consumes them. A second job packages the same `.debs` as the local-dev
container image.

```
build-freeswitch-deb (this repo)         telephony-iac
  debian/bootstrap.sh -c <codename>        packer/freeswitch (build-vf-freeswitch-ami)
  mk-build-deps -i                  →  S3  →  -var fs_artifact_s3_path=<tarball key>
  dpkg-buildpackage -b              artifact   -var artifact_s3_bucket=<bucket>
  → *.deb + SHA256SUMS + tarball              → bakes AMI → SSM /rtc/freeswitch/ami-id
  → s3://<bucket>/<prefix>/st-build-<N>/      → vm-tier/freeswitch ASG
                    ↓
  container job → ghcr.io/<owner>/rtc-freeswitch-dev:st-build-<N>
```

## Run
Actions → **build-freeswitch-deb** → Run workflow. Inputs:
- `ref` — defaults to **`v1.10_opensll3`**: the consolidated RingRx
  line **on OpenSSL 3** (native to Ubuntu 22.04 / `jammy` — no OpenSSL-1.1 shim). Pulled
  from upstream `RingRx/freeswitch` (Ryan Delgrosso). `v1.10_ringrx` is the older
  OpenSSL-1.1 production line if ever needed. Pin a SHA for a reproducible release.
- `st_build` — build number (e.g. `42` → `st-build-42` tag + artifact path).
- `ubuntu_codename` — `jammy` (match the AMI base in telephony-iac).
- `s3_bucket` / `s3_prefix` — the VF artifacts bucket (`freeswitch/ringrx` prefix).
- `publish_image` — push the dev container image to GHCR. Off = build + smoke-test only.

Output (step summary): the `fs_artifact_s3_path` to pass to the telephony-iac
`build-vf-freeswitch-ami` workflow, and the container image reference.

## Dev container image
`docker/st-dev/Dockerfile` installs the `.debs` from the `build` job into
`ubuntu:22.04` — same artifacts, same `SHA256SUMS`, same base as the AMI, so the
container and the AMI carry identical FreeSWITCH builds. Package selection matches
telephony-iac `ansible/roles/freeswitch/tasks/install.yml`, plus
`freeswitch-conf-vanilla` so the image boots standalone; mount over
`/etc/freeswitch` to supply real config.

`linux/amd64` only — `debian/bootstrap.sh` declares `Architecture: amd64 armhf`
throughout with no `arm64`, so a multi-arch manifest needs packaging changes first.

Tags are always `st-build-<N>`, never `latest` — devstacks pin the build number.

`docker/st-dev/smoke.sh <image>` boots the image and asserts the VoiceFabric module
set loads. It runs in CI and locally. `mod_azure_tts` / `mod_azure_transcribe` have
no source in this tree and are expected to be absent; any other missing or
non-loading module fails the job.

## Prereqs (one-time)
1. **Artifacts S3 bucket** — TF-managed in telephony-iac (follow-up; the FS role +
   packer read `artifact_s3_bucket`).
2. **OIDC role** — an AWS IAM role with a federated trust for
   `repo:DPrater-ST/freeswitch:environment:artifacts`, permissions scoped to
   `s3:PutObject`/`s3:GetObject` on `arn:aws:s3:::<bucket>/<prefix>/*`. Put its ARN
   in this repo's **`artifacts`** Environment as secret `AWS_ARTIFACTS_ROLE_ARN`.

No prereq for the container image: it pushes to GHCR under this repo's owner with
the built-in `GITHUB_TOKEN` (`packages: write`). ECR would need a second IAM role —
the artifacts role is scoped to S3.

## ⚠️ Governance
This is a **public, personal-account** fork. The OIDC role is environment-scoped +
the workflow is dispatch-only, but a production build source should live in the org
(`servicetitan/freeswitch`, currently an empty placeholder). Plan to mirror this
fork there and move the pipeline. The build itself is fiddly (see the
`deb11_compile_issues` branch) — expect to tune deps/modules per codename.
