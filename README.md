VCActivator (from Brchi) should be already running (single instance of VCActivator will keep all vietcong.exe instances alive)

VCScraper should be already running on minimized vanilla Vietcong, connected to the desired server.

This program wil not work correctly if Spectator delay is set to 5 seconds or lower.

It is recommended to use a separate copy of Vietcong game for the scraper and streamer. Vanilla game, windowed, lowest resolution and disabled sounds for VCScraper. HD Remaster for streamer.

Run vietcong.exe (preferably HD Remaster version 1.25 or above) run the streamer game instance in high-res window (720p for example) so it does not cover your whole screen,
or you can use fullscreen on dual monitor setup if you wish. This way you can see the concole window which will tell you why the camera changed to the specific player,
so you know IN ADVANCE what will happen in game (John kills Henry, Joe takes the VC flag...)

Inject the receiver.dll

Connect to the desired server and log in as a spectator. Use arrow keys to select "Player cameras" and stay there.

/*Delay detection not yet implemented

Now the program detects the spectator delay (it compares the total number of kills made by players in both game instances (Scraper - live, Receiver - delayed) and detects delta time.

For this to work, at least one kill has to be made, and you have to wait at least the delay time (up to 180 s based on the server settings).
*/

Connect your OBS studio to the streamer game instance window and show the world ;)

Move your mouse to set good camera angles. Uuse your beautiful voice to comment the gameplay with the help of the info from the console window.
