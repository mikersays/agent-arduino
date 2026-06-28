# UNO Q App example — Linux/Python side.
# Toggles the MCU's built-in LED once per second by calling the function the
# sketch exposed via Bridge.provide("set_led_state", ...).
#
# Run with:  arduino-app-cli app start "LED Toggle"

from arduino.app_utils import *
import time

led_state = False


def loop():
    global led_state
    time.sleep(1)
    led_state = not led_state
    Bridge.call("set_led_state", led_state)


# App.run starts the bridge and repeatedly invokes user_loop.
App.run(user_loop=loop)
