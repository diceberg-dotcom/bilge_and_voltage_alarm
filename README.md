# bilge_and_voltage_alarm
This is using an old Huzzah Feather with wifi. (Adafruit HUZZAH Feather ESP8266) but you can adjust this for any 3.3v microcontroler. This project sends an SMS to up to two phone numbers if any of two conditions happen:
If the bilge float switch circuit is closed for more than 3 seconds
If the voltage drops to 11.4v or below for 10 minutes

Note that this uses a 12v input. Intented for a marine battery on a boat. I would recommend wiring with a fuse direct to the battery so that you are always monitoring water and voltage.

Note that this is set for a sustained voltage drop of 11.4v or lower. That is very low and will start to harm the battery. 

You will need a float switch like this:
https://www.amazon.com/dp/B0CZM6THZW?ref=ppx_yo2ov_dt_b_fed_asin_title

But it is important that you change the direction of the float as they come as Closed Loop in default. (Meaning when there is no water the switch is closed and current is flowing.) This default setting will draw more current so this is NOT how the code here is written. You could probably also use a bilge pump float switch instead. (for my purpose I wanted it higher than the floor of the bilge so the alerm only activates in emergencies. In my use case, in normal conditions the bilge pumps should never allow to water to get so high as the SMS alarm is triggered.


