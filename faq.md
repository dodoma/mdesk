### Music PI FAQ

 

##### How to setting network？

- For wired networks.

  The Raspberry Pi automatically connects to the network when the network cable is plugged in, and the IP is obtained dynamically. The mpocket phone app can use the RPI source while they under the same LAN.

- For wireless networks.

  Burns the Music Pi image, after booting up, connect your phone  to the open hotspot "AVM", and then opens the mpocket app to configure the wireless network hotspot name and password. Wait for the Raspberry Pi to restart before you can use the RPI source.

##### How to update the media library in the Music Pi？

- In the LAN, through the shared address displayed on the mpocket device interface, access the samba service provided by the sound source, and manage media files (user name "pi" with password "avm")
- Connect the USB disk containing media files to the Raspberry Pi, and update the media files through the mpocket app.

 

##### Notes

1. The Music Pi will index the newly copied media files. It takes a long time to create an index (rpi 3B+ processes about 1G media files in one minute). For a large number of media files copied in, please wait patiently for the first index to complete.
2. For a better user experience, the mobile app will synchronize the index file of the currently selected media library in Music Pie in real time, as well as the cover pictures of singers and albums. Please wait patiently for data synchronization when using it for the first time.
3. Music Pi  can connect through ssh by user "pi" with password "raspberry", journalctl -u avm -f to view the audio source logs in real time.