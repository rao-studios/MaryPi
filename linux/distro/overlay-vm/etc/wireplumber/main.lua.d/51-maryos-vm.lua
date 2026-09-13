-- MaryOS in Apple's Virtualization framework only (distro/overlay-vm). WirePlumber reads
-- main.lua.d in name order: this runs after 40-device-defaults.lua and 50-alsa-config.lua
-- have made the tables it changes, and before 90-enable-all.lua starts them.

-- The Mac's own volume decides how loud the VM is, as it does for any Mac app, so the
-- guest's speaker starts at full volume. WirePlumber starts a new device at
-- 0.4^3 = 0.064 (40% in wpctl, about -24 dB), which the Mac then turns down again.
device_defaults.properties["default-volume"] = 1.0

-- In a virtual machine WirePlumber gives ALSA nodes a longer period and more headroom
-- (vm.node.defaults in 50-alsa-config.lua) to keep a virtual sound card from running dry.
-- It knows a VM only by QEMU or VMware in the DMI tables, and a guest started by
-- VZLinuxBootLoader has no DMI tables, so the virtio sound card gets those settings here.
table.insert(alsa_monitor.rules, {
  matches = {
    { { "node.name", "matches", "alsa_output.*" } },
    { { "node.name", "matches", "alsa_input.*" } },
  },
  apply_properties = {
    ["api.alsa.period-size"] = 1024,
    ["api.alsa.headroom"] = 8192,
  },
})

-- Apple's virtio sound device plays on PCM 0 and records on PCM 1, but PipeWire's default
-- profile set looks only at device 0, so the microphone never became a source and whatever
-- recorded, Mary's wake word included, was handed the speaker's monitor instead. The card
-- gets a profile set that names both devices. It is matched by its ALSA name: alsa.driver_name
-- is only added by the card itself, after these rules have run.
table.insert(alsa_monitor.rules, {
  matches = { { { "api.alsa.card.name", "equals", "VirtIO SoundCard" } } },
  apply_properties = {
    ["device.profile-set"] = "/usr/share/maryos/alsa-card-profile/virtio-snd.conf",
  },
})
