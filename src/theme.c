/*
 * Copyright (C) 2008 nsf
 */

#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <unistd.h>
#include "logger.h"
#include "theme.h"

static void free_imlib_font(Imlib_Font font);
static void free_imlib_image(Imlib_Image img);
static uint figure_out_placement(const char *str);
static uint figure_out_align(const char *str);
static int parse_key_value(const char *key, const char *value, struct theme *t);
static int parse_line(char *line, struct theme *t);
static void parse_color(struct color *c, const char *value);
static uchar hex_to_dec(uchar c);
static int load_and_parse_theme(struct theme *t);
static void calculate_values(struct theme *t);

struct theme *load_theme(const char *dir)
{
	struct theme *t = xmallocz(sizeof(struct theme));
	t->themedir = xstrdup(dir);
	if (!load_and_parse_theme(t)) {
		free_theme(t);
		return 0;
	}
	calculate_values(t);
	return t;
}

void free_theme(struct theme *t)
{
	if (t->name) xfree(t->name);
	if (t->author) xfree(t->author);
	if (t->elements) xfree(t->elements);
	if (t->themedir) xfree(t->themedir);
	if (t->clock.format) xfree(t->clock.format);

#define SAFE_FREE_IMG(img) if (img) free_imlib_image(img)
#define SAFE_FREE_IMG2(img) SAFE_FREE_IMG(img[0]); SAFE_FREE_IMG(img[1])
#define SAFE_FREE_FONT(font) if (font) free_imlib_font(font)

	/* general */
	SAFE_FREE_IMG(t->separator_img);
	SAFE_FREE_IMG(t->tile_img);

	/* clock */
	SAFE_FREE_IMG(t->clock.left_img);	
	SAFE_FREE_IMG(t->clock.tile_img);
	SAFE_FREE_IMG(t->clock.right_img);
	SAFE_FREE_FONT(t->clock.font);

	/* taskbar */
	SAFE_FREE_IMG(t->taskbar.default_icon_img);
	SAFE_FREE_IMG2(t->taskbar.left_img);
	SAFE_FREE_IMG2(t->taskbar.tile_img);
	SAFE_FREE_IMG2(t->taskbar.right_img);
	SAFE_FREE_FONT(t->taskbar.font);

	/* desktop switcher */
	SAFE_FREE_IMG(t->switcher.separator_img);
	SAFE_FREE_IMG2(t->switcher.left_corner_img);
	SAFE_FREE_IMG2(t->switcher.right_corner_img);
	SAFE_FREE_IMG2(t->switcher.left_img);
	SAFE_FREE_IMG2(t->switcher.tile_img);
	SAFE_FREE_IMG2(t->switcher.right_img);
	SAFE_FREE_FONT(t->switcher.font);

	xfree(t);
}

int theme_is_valid(struct theme *t)
{
	if (!t->elements) {
		LOG_WARNING("elements specification missing");
		return 0;
	}

	if (!is_element_in_theme(t, 'b')) {
		LOG_WARNING("taskbar element missing, it is necessary to place taskbar somewhere");
		return 0;
	}

	if (is_element_in_theme(t, 's')) {
		/* check desktop switcher */
		if (!t->switcher.font || 
		    !t->switcher.tile_img[BSTATE_IDLE] ||
		    !t->switcher.tile_img[BSTATE_PRESSED])
		{
			LOG_WARNING("one or more desktop switcher images or fonts are missing");
			return 0;
		}
	} 
	if (is_element_in_theme(t, 'b')) {
		/* check taskbar */
		if (!t->taskbar.font ||
		    !t->taskbar.tile_img[BSTATE_IDLE] ||
		    !t->taskbar.tile_img[BSTATE_PRESSED])
		{
			LOG_WARNING("one or more taskbar images or fonts are missing");
			return 0;
		}

		if (t->taskbar.icon_h != 0 &&
		    t->taskbar.icon_w != 0 &&
		    !t->taskbar.default_icon_img) 
		{
			LOG_WARNING("taskbar icon size specified, but default taskbar icon image is missing");
			return 0;
		}
	} 
	if (is_element_in_theme(t, 't')) {
		/* check icon tray */
		if (!t->tray_icon_h ||
		    !t->tray_icon_w)
		{
			LOG_WARNING("tray icon sizes are missing");
			return 0;
		}
	} 
	if (is_element_in_theme(t, 'c')) {
		/* check clock */
		if (!t->clock.font ||
		    !t->clock.tile_img)
		{
			LOG_WARNING("one or more clock images or fonts are missing");
			return 0;
		}

		if (!t->clock.format) {
			LOG_WARNING("clock format is missing");
			return 0;
		}
	}
	return 1;
}

int is_element_in_theme(struct theme *t, char e)
{
	return (strchr(t->elements, e) != 0);
}

void theme_remove_element(struct theme* t, char e)
{
	char *p, *c;
	p = c = strchr(t->elements, e);
	if (!c)
		return;

	while (*p) {
		*p = *++c;
		p++;
	}
}

/**************************************************************************
  free helpers
**************************************************************************/

static void free_imlib_font(Imlib_Font font)
{
	imlib_context_set_font(font);
	imlib_free_font();
}

static void free_imlib_image(Imlib_Image img)
{
	imlib_context_set_image(img);
	imlib_free_image();
}

/**************************************************************************
  string to enum converters
**************************************************************************/

static uint figure_out_placement(const char *str)
{
	if (!strcmp("top", str)) {
		return PLACE_TOP;
	} else if (!strcmp("bottom", str)) {
		return PLACE_BOTTOM;
	}
	return 0;
}

static uint figure_out_align(const char *str)
{
	if (!strcmp("left", str)) {
		return TALIGN_LEFT;
	} else if (!strcmp("center", str)) {
		return TALIGN_CENTER;
	} else if (!strcmp("right", str)) {
		return TALIGN_RIGHT;
	}
	return 0;
}

/**************************************************************************
  evil slow parser (TODO: rewrite with hash table?)
**************************************************************************/

static int parse_key_value(const char *key, const char *value, struct theme *t)
{
	char buf[4096];

#define CMP(str) if (!strcmp(str, key))
#define ECMP(str) else CMP(str)
#define DODIR if (value[0] == '/') snprintf(buf, sizeof(buf), "%s", value); else snprintf(buf, sizeof(buf), "%s/%s", t->themedir, value)
#define SAFE_LOAD_IMAGE(img) DODIR; img = imlib_load_image(buf); if (!img) do { LOG_WARNING("failed to load image: %s", buf); return 0; } while (0)
#define SAFE_LOAD_FONT(font) font = imlib_load_font(value); if (!font) do { LOG_WARNING("failed to load font: %s", value); return 0; } while (0)
#define PARSE_INT(un) if (1 != sscanf(value, "%d", &un)) do { LOG_WARNING("failed to parse integer: %s", value); return 0; } while (0)

	/* -------------------------- general ---------------------- */
	CMP("name") {
		t->name = xstrdup(value);
	} ECMP("author") {
		t->author = xstrdup(value);
	} ECMP("elements") {
		t->elements = xstrdup(value);
	} ECMP("version_major") {
		PARSE_INT(t->version_major);
	} ECMP("version_minor") {
		PARSE_INT(t->version_minor);
	} ECMP("placement") {
		t->placement = figure_out_placement(value); 
	} ECMP("tile_img") {
		SAFE_LOAD_IMAGE(t->tile_img);
	} ECMP("separator_img") {
		SAFE_LOAD_IMAGE(t->separator_img);
	} ECMP("tray_icon_w") {
		PARSE_INT(t->tray_icon_w);
	} ECMP("tray_icon_h") {
		PARSE_INT(t->tray_icon_h);
	/* ---------------------------- clock ----------------------- */
	} ECMP("clock_right_img") {
		SAFE_LOAD_IMAGE(t->clock.right_img);
	} ECMP("clock_tile_img") {
		SAFE_LOAD_IMAGE(t->clock.tile_img);
	} ECMP("clock_left_img") {
		SAFE_LOAD_IMAGE(t->clock.left_img);
	} ECMP("clock_font") {
		SAFE_LOAD_FONT(t->clock.font);
	} ECMP("clock_text_color") {
		parse_color(&t->clock.text_color, value);
	} ECMP("clock_text_offset_x") {
		PARSE_INT(t->clock.text_offset_x);
	} ECMP("clock_text_offset_y") {
		PARSE_INT(t->clock.text_offset_y);
	} ECMP("clock_text_padding") {
		PARSE_INT(t->clock.text_padding);
	} ECMP("clock_text_align") {
		t->clock.text_align = figure_out_align(value);
	} ECMP("clock_space_gap") {
		PARSE_INT(t->clock.space_gap);
	} ECMP("clock_format") {
		t->clock.format = xstrdup(value);
	/* ------------------------------ taskbar ----------------------- */
	} ECMP("tb_right_idle_img") {
		SAFE_LOAD_IMAGE(t->taskbar.right_img[BSTATE_IDLE]);
	} ECMP("tb_tile_idle_img") {
		SAFE_LOAD_IMAGE(t->taskbar.tile_img[BSTATE_IDLE]);
	} ECMP("tb_left_idle_img") {
		SAFE_LOAD_IMAGE(t->taskbar.left_img[BSTATE_IDLE]);

	} ECMP("tb_right_pressed_img") {
		SAFE_LOAD_IMAGE(t->taskbar.right_img[BSTATE_PRESSED]);
	} ECMP("tb_tile_pressed_img") {
		SAFE_LOAD_IMAGE(t->taskbar.tile_img[BSTATE_PRESSED]);
	} ECMP("tb_left_pressed_img") {
		SAFE_LOAD_IMAGE(t->taskbar.left_img[BSTATE_PRESSED]);

	} ECMP("tb_separator_img") {
		SAFE_LOAD_IMAGE(t->taskbar.separator_img);
	
	} ECMP("tb_default_icon_img") {
		SAFE_LOAD_IMAGE(t->taskbar.default_icon_img);

	} ECMP("tb_font") {
		SAFE_LOAD_FONT(t->taskbar.font);
	
	} ECMP("tb_text_color_idle") {
		parse_color(&t->taskbar.text_color[BSTATE_IDLE], value);
	} ECMP("tb_text_color_pressed") {
		parse_color(&t->taskbar.text_color[BSTATE_PRESSED], value);

	} ECMP("tb_text_offset_x") {
		PARSE_INT(t->taskbar.text_offset_x);
	} ECMP("tb_text_offset_y") {
		PARSE_INT(t->taskbar.text_offset_y);
	} ECMP("tb_text_align") {
		t->taskbar.text_align = figure_out_align(value);
	} ECMP("tb_icon_offset_x") {
		PARSE_INT(t->taskbar.icon_offset_x);
	} ECMP("tb_icon_offset_y") {
		PARSE_INT(t->taskbar.icon_offset_y);
	} ECMP("tb_icon_w") {
		PARSE_INT(t->taskbar.icon_w);
	} ECMP("tb_icon_h") {
		PARSE_INT(t->taskbar.icon_h);
	} ECMP("tb_space_gap") {
		PARSE_INT(t->taskbar.space_gap);
	/* ----------------------- switcher ----------------------- */
	} ECMP("ds_left_corner_idle_img") {
		SAFE_LOAD_IMAGE(t->switcher.left_corner_img[BSTATE_IDLE]);
	} ECMP("ds_right_corner_idle_img") {
		SAFE_LOAD_IMAGE(t->switcher.right_corner_img[BSTATE_IDLE]);

	} ECMP("ds_left_corner_pressed_img") {
		SAFE_LOAD_IMAGE(t->switcher.left_corner_img[BSTATE_PRESSED]);
	} ECMP("ds_right_corner_pressed_img") {
		SAFE_LOAD_IMAGE(t->switcher.right_corner_img[BSTATE_PRESSED]);

	} ECMP("ds_right_idle_img") {
		SAFE_LOAD_IMAGE(t->switcher.right_img[BSTATE_IDLE]);
	} ECMP("ds_tile_idle_img") {
		SAFE_LOAD_IMAGE(t->switcher.tile_img[BSTATE_IDLE]);
	} ECMP("ds_left_idle_img") {
		SAFE_LOAD_IMAGE(t->switcher.left_img[BSTATE_IDLE]);
	
	} ECMP("ds_right_pressed_img") {
		SAFE_LOAD_IMAGE(t->switcher.right_img[BSTATE_PRESSED]);
	} ECMP("ds_tile_pressed_img") {
		SAFE_LOAD_IMAGE(t->switcher.tile_img[BSTATE_PRESSED]);
	} ECMP("ds_left_pressed_img") {
		SAFE_LOAD_IMAGE(t->switcher.left_img[BSTATE_PRESSED]);
	} ECMP("ds_separator_img") {
		SAFE_LOAD_IMAGE(t->switcher.separator_img);
	} ECMP("ds_font") {
		SAFE_LOAD_FONT(t->switcher.font);

	} ECMP("ds_text_color_idle") {
		parse_color(&t->switcher.text_color[BSTATE_IDLE], value);
	} ECMP("ds_text_color_pressed") {
		parse_color(&t->switcher.text_color[BSTATE_PRESSED], value);
		
	} ECMP("ds_text_offset_x") {
		PARSE_INT(t->switcher.text_offset_x);
	} ECMP("ds_text_offset_y") {
		PARSE_INT(t->switcher.text_offset_y);
	} ECMP("ds_text_padding") {
		PARSE_INT(t->switcher.text_padding);
	} ECMP("ds_text_align") {
		t->switcher.text_align = figure_out_align(value);
	} ECMP("ds_space_gap") {
		PARSE_INT(t->switcher.space_gap);
	} else {
		LOG_WARNING("unknown key: %s, and value: %s", key, value);
		return 0;
	}

	return 1;
}

static int parse_line(char *line, struct theme *t)
{
	/* TODO: error checks */
	int len = strlen(line);
	char *key, *value;

	line[--len] = '\0'; /* remove \n sign */

	key = line;
	while (isspace(*key))
		key++;
	value = key;
	while (!isspace(*value))
		value++;
	*value++ = '\0';
	while (isspace(*value))
		value++;
	
	return parse_key_value(key, value, t);
}

static int load_and_parse_theme(struct theme *t)
{
	char buf[4096];
	snprintf(buf, sizeof(buf), "%s/theme", t->themedir);

	FILE *f = fopen(buf, "r");
	if (!f) {
		return 0;
	}

	for (;;) {
		fgets(buf, sizeof(buf), f);
		if (feof(f))
			break;
		if (buf[0] == '\0' || buf[0] == '\n' || buf[0] == '#')
			continue;
		if (!parse_line(buf, t)) {
			fclose(f);
			LOG_WARNING("fatal loading error");
			return 0;
		}
	}

	fclose(f);
	return 1;
}

static void calculate_values(struct theme *t)
{
	imlib_context_set_image(t->tile_img);
	t->height = imlib_image_get_height();
}

static uchar hex_to_dec(uchar c)
{
	if (isxdigit(c)) {
		if (isdigit(c))
			return ((char)c - '0');
		c = tolower(c);
		return ((char)c - 'a' + 10);
	}
	return 15;
}

static void parse_color(struct color *c, const char *value)
{
	/* red */
	c->r = 16 * hex_to_dec(*value++);
	c->r += hex_to_dec(*value++);
	/* green */
	c->g = 16 * hex_to_dec(*value++);
	c->g += hex_to_dec(*value++);
	/* blue */
	c->b = 16 * hex_to_dec(*value++);
	c->b += hex_to_dec(*value++);
}