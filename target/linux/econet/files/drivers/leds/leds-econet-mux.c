// SPDX-License-Identifier: GPL-2.0-only
/*
 * LEDs on EcoNet/TrendChip pins that are shared between a GPIO and a hardware
 * function (e.g. switch port LEDs), selected by one bit each in the SCU GPIO
 * share register.
 *
 * Each LED starts with the "switch" trigger, which leaves the pin to the
 * hardware. Choosing any other trigger (or none) hands the pin to the GPIO,
 * so Linux drives the LED; selecting "switch" again gives it back.
 */

#include <linux/bits.h>
#include <linux/gpio/consumer.h>
#include <linux/leds.h>
#include <linux/mfd/syscon.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/regmap.h>

#define ECONET_GPIO_SHR		0x860

struct econet_mux_led {
	struct led_classdev cdev;
	struct gpio_desc *gpio;
	struct regmap *scu;
	u32 bit;
};

static struct led_hw_trigger_type econet_mux_hw_type;

static struct econet_mux_led *to_mux_led(struct led_classdev *cdev)
{
	return container_of(cdev, struct econet_mux_led, cdev);
}

static int econet_mux_led_set(struct led_classdev *cdev,
			      enum led_brightness value)
{
	gpiod_set_value_cansleep(to_mux_led(cdev)->gpio, value != LED_OFF);

	return 0;
}

/* "switch" trigger active: the pin belongs to the hardware function */
static int econet_mux_trig_activate(struct led_classdev *cdev)
{
	struct econet_mux_led *led = to_mux_led(cdev);

	return regmap_set_bits(led->scu, ECONET_GPIO_SHR, BIT(led->bit));
}

static void econet_mux_trig_deactivate(struct led_classdev *cdev)
{
	struct econet_mux_led *led = to_mux_led(cdev);

	gpiod_set_value_cansleep(led->gpio, 0);
	regmap_clear_bits(led->scu, ECONET_GPIO_SHR, BIT(led->bit));
}

static struct led_trigger econet_mux_trigger = {
	.name = "switch",
	.activate = econet_mux_trig_activate,
	.deactivate = econet_mux_trig_deactivate,
	.trigger_type = &econet_mux_hw_type,
};

static int econet_mux_leds_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct regmap *scu;
	int ret;

	scu = syscon_regmap_lookup_by_phandle(dev->of_node, "econet,scu");
	if (IS_ERR(scu))
		return dev_err_probe(dev, PTR_ERR(scu), "missing econet,scu\n");

	ret = devm_led_trigger_register(dev, &econet_mux_trigger);
	if (ret)
		return ret;

	device_for_each_child_node_scoped(dev, child) {
		struct led_init_data init_data = { .fwnode = child };
		struct econet_mux_led *led;

		led = devm_kzalloc(dev, sizeof(*led), GFP_KERNEL);
		if (!led)
			return -ENOMEM;

		ret = fwnode_property_read_u32(child, "econet,mux-bit",
					       &led->bit);
		if (ret || led->bit > 31)
			return dev_err_probe(dev, -EINVAL,
					     "bad econet,mux-bit\n");

		led->gpio = devm_fwnode_gpiod_get(dev, child, NULL,
						  GPIOD_OUT_LOW, NULL);
		if (IS_ERR(led->gpio))
			return dev_err_probe(dev, PTR_ERR(led->gpio),
					     "failed to get gpio\n");

		led->scu = scu;
		led->cdev.max_brightness = 1;
		led->cdev.brightness_set_blocking = econet_mux_led_set;
		led->cdev.trigger_type = &econet_mux_hw_type;
		led->cdev.default_trigger = econet_mux_trigger.name;

		ret = devm_led_classdev_register_ext(dev, &led->cdev,
						     &init_data);
		if (ret)
			return ret;
	}

	return 0;
}

static const struct of_device_id econet_mux_leds_of_match[] = {
	{ .compatible = "econet,mt7510-mux-leds" },
	{ }
};
MODULE_DEVICE_TABLE(of, econet_mux_leds_of_match);

static struct platform_driver econet_mux_leds_driver = {
	.probe = econet_mux_leds_probe,
	.driver = {
		.name = "leds-econet-mux",
		.of_match_table = econet_mux_leds_of_match,
	},
};
module_platform_driver(econet_mux_leds_driver);

MODULE_DESCRIPTION("EcoNet LEDs on GPIO/hardware shared pins");
MODULE_LICENSE("GPL");
