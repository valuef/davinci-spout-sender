# Davinci Resolve Spout Sender
OpenFX Spout sender for Davinci Resolve.


https://github.com/user-attachments/assets/12aab686-c4bd-4b89-b273-9d3e0e335175


## Contributing
If you're gonna do changes with the purpose of merging upstream, talk to me first: https://value.gay
Otherwise it's likely gonna get ignored.

## Installation
1. Download a release
2. Place the 'SpoutSender.ofx.bundle' folder in 'C:\Program Files\Common Files\OFX\Plugins'
3. Restart davinci
4. Put the Spout Sender OpenFX filter on a clip in the timeline, adjustment clip, or the timeline color grade.
5. Change the name of the spout sender.

## Known Bugs
* Output is upside down. Not fixing this currently as that would require CPU work to flip the incoming frame. Flip in receiver instead. Ping me if there's a way in D3D11 to flip on resource upload.
* Dead senders will not clean up properly. This is likely due to Davinci Resolve not destroying instances of the plugin.
* Might not handle RGB/Alpha formats properly.
* Only supports davinci resolve. No desire to support other programs.
* Might not properly work with dual GPU setups (e.g laptops)

## Building
Build with MSVC on Visual Studio. I build it with VS2022.

## License
MIT
