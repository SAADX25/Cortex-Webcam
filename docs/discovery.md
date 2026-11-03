# Automatic Studio connection

Open `CortexStudio.exe` on the PC, then open Cortex Sender on a phone attached to the same LAN over Wi-Fi. Desktop 0.4 starts reception automatically and keeps it armed while open or minimized. There is no Start/Stop studio state to strand discovery. The optional Reconnect phone button refreshes the receiver; it returns to listening automatically. Closing the PC app still stops reception. The phone discovers Studio, confirms compatible settings, requests camera permission on first use, and starts streaming without entering an IP address. Android permission requirements still apply; opening the app does not bypass camera consent.

Power Saver and backgrounding the activity keep the foreground service, discovery heartbeats, and the existing hardware camera/encoder pipeline alive. Pause stops the phone until the next visit or explicit reconnect; bringing the app back to the foreground arms automatic connection again. A lost Wi-Fi network or Studio service stops the automatic camera pipeline and initiates rediscovery. A changed video port or reset receiver frame counter also restarts the camera stream automatically. Transient camera/transport failures retry with backoff; camera permissions are never bypassed. The PC retries video/control bind failures every two seconds, so releasing a conflicting port is sufficient to recover.

## Service and control protocol

Studio uses Windows' native `DnsServiceRegister`/`DnsServiceDeRegister` APIs to publish `_cortexcam._udp.local`. No Bonjour daemon or bundled mDNS dependency is required. The OS handles mDNS records, interfaces, announcements, queries and withdrawal. The instance name includes the PC hostname and process ID to avoid simultaneous-process collisions. Android uses `NsdManager`, serializing service resolutions and validating TXT metadata:

| Record | Value |
| --- | --- |
| Service | `_cortexcam._udp` |
| SRV port | UDP control port, default **5005** |
| TXT `v` | `1` |
| TXT `codec` | `h264` |
| TXT `transport` | `rtp` |
| TXT `video` | RTP port hint, default `5004`; handshake is authoritative |

The PC advertises all eligible interfaces, allowing a wired PC on the same LAN as a Wi-Fi phone. Android 33+ scopes discovery to a Wi-Fi `Network`; earlier supported Android versions browse with the legacy NSD API and bind the handshake and RTP sockets to the discovered Wi-Fi network. Only IPv4 endpoints are selected to match the existing receiver. TXT metadata alone never starts video.

```text
Phone → PC control UDP:
CORTEX/1 HELLO <32 lowercase hex nonce> H264 1920 1080 <30|60>\n

PC → Phone:
CORTEX/1 READY <same nonce> <RTP port> H264 1920 1080 <same fps>\n
or CORTEX/1 WAIT <same nonce>\n
or CORTEX/1 BUSY <same nonce>\n
```

READY is returned only when the receiver has bound its video socket and initialized its hardware decoder context. It confirms protocol/settings compatibility; camera capability and actual D3D11 decode are still validated when media starts. The phone validates the nonce, peer endpoint, port, codec, geometry and frame rate. Each exchange has up to two 450 ms receive attempts on a worker thread. Discovery resolves one candidate at a time and chooses the first compatible, ready Studio, keeping it selected until loss/failure rather than switching between advertisements.

If mDNS resolution stalls or fails, Android sends `CORTEX/1 DISCOVER <nonce>` to the Wi-Fi subnet broadcast and limited broadcast address on control UDP 5005. A ready Studio responds `CORTEX/1 OFFER <nonce>` without acquiring a lease; Android then performs the regular unicast HELLO handshake. A previously verified PC address is also tried on the current Wi-Fi network. Manual connection uses a PC address and **control** port and performs the same handshake, rather than blindly sending RTP. Handshake and media sockets are bound to the Wi-Fi network.

After heartbeats the phone queries `CORTEX/1 STATUS <nonce>`. The leased peer receives `CORTEX/1 VIDEO <nonce> <decoded-frames> <received-packets>`. Growing decoded-frame counters confirm live video at the PC; merely opening the phone camera does not. These counters are progress feedback, not a latency measurement. An mDNS advertisement expiry alone does not interrupt a healthy heartbeat.

The phone repeats HELLO approximately every two seconds as a heartbeat. Studio grants one peer IPv4 an eight-second renewable lease and installs that peer's RTP address filter **before** sending READY. Other peers get BUSY. Three consecutive failed heartbeat exchanges trigger rediscovery. On service/network loss, auto streaming stops earlier. Stopping the phone releases the lease by expiry; there is no explicit RELEASE command. Manual receiver use remains available when there is no automatic lease. This lease prevents accidental competition; mDNS, the nonce and IP filtering are not authentication or encryption.

Discovery and media remain separate modules. Media remains H.264 RTP/UDP, with Android Camera2 → hardware encoder Surface and Windows D3D11VA GPU frames. Control-plane ASCII is never used to carry camera frames.

## PC configuration

```powershell
# Video 5004, no fixed sender allowlist, continuous operation, control 5005:
.\cortex-receive.exe

# Video 6000, allow only this phone, run 300 seconds, control 6001:
.\cortex-receive.exe 6000 192.168.1.42 300 6001
```

Allow mDNS and the chosen control/video UDP ports on the intended Windows Firewall LAN profile. Desktop's **Allow phone** helper requests administrator consent once and creates an executable-specific UDP rule for the selected video/control ports, restricted to LocalSubnet. Public profiles are included because Windows may classify a home Ethernet network as Public. No firewall is disabled. Guest Wi-Fi/client isolation or VPN routing can still prevent access. DNS registration errors leave broadcast and direct handshakes working. The native CLI does not change firewall rules.

Android currently targets API 35 with min API 29 and adds `ACCESS_NETWORK_STATE` to the existing Internet/camera/foreground-service permissions. Android 17 local-network rules apply when raising the target to API 37 and require a permission/picker migration; this change does not raise the target. Devices with developer-enabled local-network restrictions may also require local-network access configuration.

## Validation

Windows CTest covers readiness, strict handshake parsing, lease-free broadcast offers, settings/nonce echo, malformed requests, competing peers, lease renewal and expiry, alongside existing RTP/source-manager tests. The native Studio integration test holds the video/control ports busy, releases them, and verifies recovery without a Start action; it also checks automatic rearming after reconnect. Android unit tests cover its side of the wire contract and receiver progress feedback. `scripts/DiscoverySmokeTest.ps1` runs the diagnostic receiver and checks UDP READY/BUSY behavior and native mDNS. Its optional `-FFmpegExe` argument also sends synthetic video and verifies actual GPU NV12 decode.

Android physical-device NSD → camera → Wi-Fi → PC operation still needs an on-device acceptance run. Local multicast and synthetic-video checks do not establish behavior across a particular access point or its firewall policy.

References: [Android NSD](https://developer.android.com/reference/android/net/nsd/NsdManager), [Windows service registration](https://learn.microsoft.com/en-us/windows/win32/api/windns/nf-windns-dnsserviceregister), [Windows mDNS registration fields](https://learn.microsoft.com/en-us/windows/win32/api/windns/ns-windns-dns_service_register_request).
