
/****************************************************************************************
 *
 * ftcSoundBar - a LyraT based fischertechnik compatible music box.
 *
 * Version 0.1
 *
 * (C) 2020 Oliver Schmied, Christian Bergschneider & Stefan Fuss
 *
 * Please run idf.py menuconfig, first
 *
 * EXAMPLE-CONFIGURATION: setting SSID & Password is helpful, but not mandatory
 * COMPONENT-CONFIGURATION/FAT FILESYSTEM SUPPORT: long filename support, UTF 8 encoding
 *
 ****************************************************************************************/

#include <esp_wifi.h>
#include <esp_event.h>
#include <nvs_flash.h>
#include <esp_log.h>
#include <audio_element.h>
#include <esp_flash_partitions.h>
#include <esp_partition.h>
#include <esp_ota_ops.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>
#include <input_key_service.h>
#include <string.h>
#include <sys/param.h>
#include <sys/unistd.h>
#include <esp_vfs.h>
#include <esp_http_server.h>
#include <cJSON.h>
#include <stdbool.h>
#include <mdns.h>
#include <esp_netif.h>
#include <periph_wifi.h>

#include "adfcorrections.h"
#include "playlist.h"
#include "pipeline.h"
#include "ftcSoundBar.h"

extern "C" {
    void app_main(void);
}

#define FIRMWARE_VERSION "v1.1"

// DEBUGREST shows detailed informations during RESTAPI calls
//#define DEBUGREST

#define CONFIG_FILE "/sdcard/ftcSoundBar.conf"
static const char *TAG = "ftcSoundBar";
#define FIRMWAREUPDATE "/sdcard/ftcSoundBar.bin"


static EventGroupHandle_t wifi_event_group;
const int CONNECTED_BIT = BIT0;

#define BUFFSIZE 1024
static char ota_write_data[BUFFSIZE + 1] = { 0 };

#define BLINK_GPIO 22

#define FILE_PATH_MAX (ESP_VFS_PATH_MAX + 128)
#define SCRATCH_BUFSIZE (10240)

typedef struct http_server_context {
    char base_path[ESP_VFS_PATH_MAX + 1];
    char scratch[SCRATCH_BUFSIZE];
} http_server_context_t;

FtcSoundBar ftcSoundBar;

/*
esp_err_t audio_element_event_handler(audio_element_handle_t self, audio_event_iface_msg_t *event, void *ctx)
{
	return ftcSoundBar.pipeline.event_handler(self, event, ctx);
}
*/


/**************************************************************************************
 *
 * simple tasks
 *
 **************************************************************************************/


void task_reboot(void *pvParameter)
{
	ESP_LOGI( TAG, "I will reboot in 0.5s...");
	vTaskDelay(500 / portTICK_RATE_MS);
	esp_restart();

}

void task_blinky(void *pvParameter)
{

    gpio_pad_select_gpio(BLINK_GPIO);
    /* Set the GPIO as a push/pull output */
    gpio_set_direction((gpio_num_t)BLINK_GPIO, GPIO_MODE_OUTPUT);
    while( 1 ) {
        /* Blink off (output low) */
        gpio_set_level((gpio_num_t)BLINK_GPIO, 0);
        vTaskDelay(250 / portTICK_RATE_MS);
        /* Blink on (output high) */
        gpio_set_level((gpio_num_t)BLINK_GPIO, 1);
        vTaskDelay(250 / portTICK_RATE_MS);
    }

}

#define OTATAG "OTA"

static void __attribute__((noreturn)) task_fatal_error()
{
    ESP_LOGE(OTATAG, "Exiting task due to fatal error...");
    (void)vTaskDelete(NULL);

    while (1) {
        ;
    }
}

void task_ota(void *pvParameter)
{
    esp_err_t err;
    /* update handle : set by esp_ota_begin(), must be freed via esp_ota_end() */
    esp_ota_handle_t update_handle = 0 ;
    const esp_partition_t *update_partition = NULL;

    ESP_LOGI(OTATAG, "Starting OTA...");

    xTaskCreate(&task_blinky, "blinky", 512, NULL, 5, &(ftcSoundBar.xBlinky) );

    const esp_partition_t *configured = esp_ota_get_boot_partition();
    const esp_partition_t *running = esp_ota_get_running_partition();

    if (configured != running) {
        ESP_LOGW(OTATAG, "Configured OTA boot partition at offset 0x%08x, but running from offset 0x%08x",
                 configured->address, running->address);
        ESP_LOGW(OTATAG, "(This can happen if either the OTA boot data or preferred boot image become corrupted somehow.)");
    }
    ESP_LOGI(OTATAG, "Running partition type %d subtype %d (offset 0x%08x)",
             running->type, running->subtype, running->address);

    FILE *f;
    f = fopen(FIRMWAREUPDATE, "rb");
    if ( f == NULL ) {
    	ESP_LOGI( TAG, "ftcSoundBar.bin not found");
    	task_fatal_error();
    }

    update_partition = esp_ota_get_next_update_partition(NULL);
    ESP_LOGI(OTATAG, "Writing to partition subtype %d at offset 0x%x",
             update_partition->subtype, update_partition->address);
    assert(update_partition != NULL);

    int binary_file_length = 0;
     /*deal with all receive packet*/
     bool image_header_was_checked = false;
     while (1) {
    	 //int data_read = esp_http_client_read(client, ota_write_data, BUFFSIZE);
         int data_read = fread( ota_write_data, 1, BUFFSIZE, f );
         ESP_LOGI( OTATAG, "data_read=%d", data_read);
         if (data_read < 0) {
             ESP_LOGE(OTATAG, "Error: file data read error");
             task_fatal_error();
         } else if (data_read > 0) {
             if (image_header_was_checked == false) {
                 esp_app_desc_t new_app_info;
                 if (data_read > sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t)) {
                     // check current version with downloading
                     memcpy(&new_app_info, &ota_write_data[sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t)], sizeof(esp_app_desc_t));
                     ESP_LOGI(OTATAG, "New firmware version: %s", new_app_info.version);

                     esp_app_desc_t running_app_info;
                     if (esp_ota_get_partition_description(running, &running_app_info) == ESP_OK) {
                         ESP_LOGI(OTATAG, "Running firmware version: %s", running_app_info.version);
                     }

                     const esp_partition_t* last_invalid_app = esp_ota_get_last_invalid_partition();
                     esp_app_desc_t invalid_app_info;
                     if (esp_ota_get_partition_description(last_invalid_app, &invalid_app_info) == ESP_OK) {
                         ESP_LOGI(OTATAG, "Last invalid firmware version: %s", invalid_app_info.version);
                     }

                     // check current version with last invalid partition
                     if (last_invalid_app != NULL) {
                         if (memcmp(invalid_app_info.version, new_app_info.version, sizeof(new_app_info.version)) == 0) {
                             ESP_LOGW(OTATAG, "New version is the same as invalid version.");
                             ESP_LOGW(OTATAG, "Previously, there was an attempt to launch the firmware with %s version, but it failed.", invalid_app_info.version);
                             ESP_LOGW(OTATAG, "The firmware has been rolled back to the previous version.");
                             task_fatal_error();
                         }
                     }

                     image_header_was_checked = true;

                     err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &update_handle);
                     if (err != ESP_OK) {
                         ESP_LOGE(OTATAG, "esp_ota_begin failed (%s)", esp_err_to_name(err));
                         task_fatal_error();
                     }
                     ESP_LOGI(OTATAG, "esp_ota_begin succeeded");
                 } else {
                     ESP_LOGE(OTATAG, "received package is not fit len");
                     task_fatal_error();
                 }
             }
             err = esp_ota_write( update_handle, (const void *)ota_write_data, data_read);
             if (err != ESP_OK) {
                 task_fatal_error();
             }
             binary_file_length += data_read;
             ESP_LOGD(OTATAG, "Written image length %d", binary_file_length);
         } else if (data_read == 0) {

                 ESP_LOGI(OTATAG, "EOF");
                 break;

        }
     }
     ESP_LOGI(OTATAG, "Total Write binary data length: %d", binary_file_length);

     fclose(f);

     err = esp_ota_end(update_handle);
     if (err != ESP_OK) {
         if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
             ESP_LOGE(OTATAG, "Image validation failed, image is corrupted");
         }
         ESP_LOGE(OTATAG, "esp_ota_end failed (%s)!", esp_err_to_name(err));
         task_fatal_error();
     }

     err = esp_ota_set_boot_partition(update_partition);
     if (err != ESP_OK) {
         ESP_LOGE(OTATAG, "esp_ota_set_boot_partition failed (%s)!", esp_err_to_name(err));
         task_fatal_error();
     }
     ESP_LOGI(OTATAG, "Prepare to restart system!");

    esp_restart();

}

/******************************************************************
 *
 * WebServer
 *
 ******************************************************************/

/**
 * @brief send header of web site
 *
 * @param req
 *
 * @return error-code
 */
static esp_err_t send_header(httpd_req_t *req )
{
	extern const unsigned char header_start[] asm("_binary_header_html_start");
    extern const unsigned char header_end[]   asm("_binary_header_html_end");
    const size_t header_size = (header_end - header_start);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send_chunk(req, (const char *)header_start, header_size);
    return ESP_OK;
}


/**
 * @brief send favicon.ico
 *
 * @param req
 *
 * @return error-code
 */
static esp_err_t send_img(httpd_req_t *req )
{
	extern const unsigned char ftcSound_start[] asm("_binary_favicon_ico_start");
    extern const unsigned char ftcSound_end[]   asm("_binary_favicon_ico_end");
    const size_t ftcSound_size = (ftcSound_end - ftcSound_start);

    httpd_resp_set_type(req, "image/ico");
    httpd_resp_send_chunk(req, (const char *)ftcSound_start, ftcSound_size);
    return ESP_OK;
}

/**
 * @brief deliver favicon.ico
 *
 * @param req
 *
 * @return error-code
 */
static esp_err_t favico_get_handler(httpd_req_t *req )
{
	send_img(req);
    httpd_resp_sendstr_chunk(req, NULL);

	return ESP_OK;
}

typedef enum {
	IMAGE_LOGO,
	IMAGE_COCKTAIL,
	IMAGE_NEXT,
	IMAGE_PREVIOUS,
	IMAGE_PLAY,
	IMAGE_STOP,
	IMAGE_SHUFFLE,
	IMAGE_REPEAT,
	IMAGE_VOLUME_UP,
	IMAGE_VOLUME_DOWN,
	IMAGE_SETUP
} image_t;


esp_err_t httpd_resp_sendstr_chunk_cr(httpd_req_t *req, const char *line ) {

	char cr[10];

	httpd_resp_sendstr_chunk( req, line );

	sprintf(cr, "\n");
	httpd_resp_sendstr_chunk( req, cr);

	return ESP_OK;
}

esp_err_t httpd_resp_send_img(httpd_req_t *req, const char* prefix, image_t image, const char *postfix )
{
	httpd_resp_sendstr_chunk(req, prefix);

	if ( image == IMAGE_LOGO ) {
    	extern const unsigned char ftcSoundbar_start[] asm("_binary_ftcsoundbarlogo_svg_start");
		extern const unsigned char ftcSoundbar_end[]   asm("_binary_ftcsoundbarlogo_svg_end");
		const size_t ftcSoundbar_size = (ftcSoundbar_end - ftcSoundbar_start);
		httpd_resp_send_chunk(req, (const char *)ftcSoundbar_start, ftcSoundbar_size);

	} else if ( image == IMAGE_COCKTAIL ) {
		extern const unsigned char cocktail_start[] asm("_binary_cocktail_svg_start");
		extern const unsigned char cocktail_end[]   asm("_binary_cocktail_svg_end");
		const size_t cocktail_size = (cocktail_end - cocktail_start);
		httpd_resp_send_chunk(req, (const char *)cocktail_start, cocktail_size);

	} else if ( image == IMAGE_NEXT ) {
		extern const unsigned char next_start[] asm("_binary_next_svg_start");
		extern const unsigned char next_end[]   asm("_binary_next_svg_end");
		const size_t next_size = (next_end - next_start);
		httpd_resp_send_chunk(req, (const char *)next_start, next_size);

	} else if ( image == IMAGE_PLAY ) {
		extern const unsigned char play_start[] asm("_binary_play_svg_start");
		extern const unsigned char play_end[]   asm("_binary_play_svg_end");
		const size_t play_size = (play_end - play_start);
		httpd_resp_send_chunk(req, (const char *)play_start, play_size);

	} else if ( image == IMAGE_PREVIOUS ) {
		extern const unsigned char previous_start[] asm("_binary_previous_svg_start");
		extern const unsigned char previous_end[]   asm("_binary_previous_svg_end");
		const size_t previous_size = (previous_end - previous_start);
		httpd_resp_send_chunk(req, (const char *)previous_start, previous_size);

	} else if ( image == IMAGE_STOP ) {
		extern const unsigned char stop_start[] asm("_binary_stop_svg_start");
		extern const unsigned char stop_end[]   asm("_binary_stop_svg_end");
		const size_t stop_size = (stop_end - stop_start);
		httpd_resp_send_chunk(req, (const char *)stop_start, stop_size);

	} else if ( image == IMAGE_VOLUME_UP ) {
		extern const unsigned char volumeup_start[] asm("_binary_volumeup_svg_start");
		extern const unsigned char volumeup_end[]   asm("_binary_volumeup_svg_end");
		const size_t volumeup_size = (volumeup_end - volumeup_start);
		httpd_resp_send_chunk(req, (const char *)volumeup_start, volumeup_size);

	} else if ( image == IMAGE_VOLUME_DOWN ) {
		extern const unsigned char volumedown_start[] asm("_binary_volumedown_svg_start");
		extern const unsigned char volumedown_end[]   asm("_binary_volumedown_svg_end");
		const size_t volumedown_size = (volumedown_end - volumedown_start);
		httpd_resp_send_chunk(req, (const char *)volumedown_start, volumedown_size);

	} else if ( image == IMAGE_SHUFFLE ) {
		extern const unsigned char shuffle_start[] asm("_binary_shuffle_svg_start");
		extern const unsigned char shuffle_end[]   asm("_binary_shuffle_svg_end");
		const size_t shuffle_size = (shuffle_end - shuffle_start);
		httpd_resp_send_chunk(req, (const char *)shuffle_start, shuffle_size);

	} else if ( image == IMAGE_REPEAT ) {
		extern const unsigned char repeat_start[] asm("_binary_repeat_svg_start");
		extern const unsigned char repeat_end[]   asm("_binary_repeat_svg_end");
		const size_t repeat_size = (repeat_end - repeat_start);
		httpd_resp_send_chunk(req, (const char *)repeat_start, repeat_size);

	} else if ( image == IMAGE_SETUP ) {
		extern const unsigned char setup_start[] asm("_binary_setup_svg_start");
		extern const unsigned char setup_end[]   asm("_binary_setup_svg_end");
		const size_t setup_size = (setup_end - setup_start);
		httpd_resp_send_chunk(req, (const char *)setup_start, setup_size);

	} else {
		return ESP_FAIL;

	}

	httpd_resp_sendstr_chunk_cr(req, postfix);

	return ESP_OK;
}

/**
 * @brief deliver styles.css
 *
 * @param req
 *
 * @return error-code
 */
static esp_err_t styles_css_get_handler(httpd_req_t *req )
{
	extern const unsigned char styles_start[] asm("_binary_styles_css_start");
    extern const unsigned char styles_end[]   asm("_binary_styles_css_end");
    const size_t styles_size = (styles_end - styles_start);

    httpd_resp_set_type(req, "text/css");
    httpd_resp_send_chunk(req, (const char *)styles_start, styles_size);

	/* Send empty chunk to signal HTTP response completion */
    httpd_resp_sendstr_chunk(req, NULL);

    return ESP_OK;
}

/**
 * @brief send footer of web site
 *
 * @param req
 *
 * @return error-code
 */
static esp_err_t send_footer(httpd_req_t *req )
{
	/*
	extern const unsigned char footer_start[] asm("_binary_footer_html_start");
    extern const unsigned char footer_end[]   asm("_binary_footer_html_end");
    const size_t footer_size = (footer_end - footer_start);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send_chunk(req, (const char *)footer_start, footer_size);
    return ESP_OK;
    */

	httpd_resp_sendstr_chunk_cr(req, "<div class=\"small\">" );
	httpd_resp_sendstr_chunk_cr(req, "	<br />" );
	httpd_resp_send_img( req, "	<p>", IMAGE_COCKTAIL, " " );
	httpd_resp_sendstr_chunk_cr(req, "		&nbsp; (C) 2020/2021 Programmierung: Christian Bergschneider & Stefan Fuss - Mitarbeit: Oliver Schmiel &nbsp;");
	httpd_resp_send_img( req, "		", IMAGE_COCKTAIL, "</p>" );
	httpd_resp_sendstr_chunk_cr(req, "	<br />" );
	httpd_resp_sendstr_chunk_cr(req, "</div>" );
	httpd_resp_sendstr_chunk_cr(req, "</body>");
	httpd_resp_sendstr_chunk_cr(req, "</html>");

	return ESP_OK;
}


/**
 * @brief deliver html site setup
 *
 * @param req
 *
 * @return error-code
 */
static esp_err_t setup_html_get_handler(httpd_req_t *req )
{
	char line[256];

	send_header(req);

	httpd_resp_sendstr_chunk_cr(req, "<table>" );
	httpd_resp_sendstr_chunk_cr(req, "	<tr height=\"20\"><td></td></tr>" );
	httpd_resp_send_img( req, "		<tr><td><a href=\"/\">", IMAGE_LOGO, "</a></td></tr>" );
	httpd_resp_sendstr_chunk_cr(req, "<table>" );
	httpd_resp_sendstr_chunk_cr(req, "<hr>" );

	httpd_resp_sendstr_chunk(req, "<table align=\"center\">" );

	sprintf(line, "<tr><td style=\"text-align:right\">firmware version:</td><td style=\"text-align:left\">%s</td></tr>", FIRMWARE_VERSION );
	httpd_resp_sendstr_chunk(req, line );

	sprintf(line, "<tr><td style=\"text-align:right\">wifi SSID:</td><td><input type=\"text\" value=\"%s\" id=\"wifi_ssid\"></td></tr>", ftcSoundBar.WIFI_SSID );
	httpd_resp_sendstr_chunk(req, line );

	sprintf(line, "<tr><td style=\"text-align:right\">pre-shared key:</td><td><input type=\"password\" value=\"%s\" id=\"wifi_password\"></td></tr>", ftcSoundBar.WIFI_PASSWORD );
	httpd_resp_sendstr_chunk(req, line );

	char checked[20] = "";
	char display[30] = "";

	if ( ftcSoundBar.TXT_AP_MODE) {
		strcpy( checked, "checked");
	} else {
		strcpy( display, "style=\"display:none\"");
	}

	sprintf( line, "<tr><td style=\"text-align:right\">txt client mode:</td><td style=\"text-align:left\"><label class=\"switch\"><input type=\"checkbox\" id=\"txt_ap_mode\" %s><span class=\"slider round\"></span></label></td></tr>", checked);
	httpd_resp_sendstr_chunk(req, line );

	// sprintf( "<div id=\"ip\" %s><tr><td style=\"text-align:right\">ip address:</td><td style=\"text-align:left\">192.168.8.100</td></tr><div>", display);
	// httpd_resp_sendstr_chunk(req, line );

	if ( access( FIRMWAREUPDATE, F_OK ) != -1 ) {
		sprintf( line, "<tr><td><button type=\"button\" onclick=\"ota()\">update firmware</button></td><td><button type=\"button\" onclick=\"save_config('%s')\">save configuration</button></td></tr>", ftcSoundBar.HOSTNAME);
		httpd_resp_sendstr_chunk(req, line );
	} else {
		sprintf( line, "<tr><td colspan=\"2\"><button type=\"button\" onclick=\"save_config('%s')\">save configuration</button></td></tr>", ftcSoundBar.HOSTNAME);
		httpd_resp_sendstr_chunk(req, line );
	}

	httpd_resp_sendstr_chunk(req, "</table><hr>" );

	send_footer(req);

	/* Send empty chunk to signal HTTP response completion */
    httpd_resp_sendstr_chunk(req, NULL);

	return ESP_OK;
}

/**
 * @brief deliver main html site play
 *
 * @param req
 *
 * @return error-code
 */
 esp_err_t root_html_get_handler(httpd_req_t *req )
{
	char line[512];

	send_header(req);

	// Logo
	httpd_resp_sendstr_chunk_cr(req, "<table>" );
	httpd_resp_sendstr_chunk_cr(req, "	<tr height=\"20\"><td></td></tr>" );
	httpd_resp_send_img( req, "		<tr><td>", IMAGE_LOGO, "</td></tr>" );
	httpd_resp_sendstr_chunk_cr(req, "<table>" );
	httpd_resp_sendstr_chunk_cr(req, "<hr>" );

	const char   active[30] = "style=\"color: #989898;\"";
	const char inactive[30] = "style=\"color: #383838;\"";

	char  playing[30], stopped[30], repeat[30], shuffle[30], error[30], paused[30];

	strcpy( playing, inactive );
	strcpy( paused,  inactive);
	strcpy( stopped, inactive);
	strcpy( repeat,  inactive );
	strcpy( shuffle, inactive );
	strcpy( error,   inactive );

	switch ( ftcSoundBar.pipeline.getState() ) {
	case AEL_STATE_RUNNING: strcpy( playing, active); break;
	case AEL_STATE_PAUSED:	strcpy( paused,  active); break;
	case AEL_STATE_ERROR:   strcpy( error,   active); break;
	default:                strcpy( stopped, active); break;
	}

	switch ( ftcSoundBar.pipeline.getMode() ) {
	case MODE_SHUFFLE:      strcpy( shuffle, active); break;
	case MODE_REPEAT:		strcpy( repeat, active);  break;
	default:                                          break;
	}

	// active track
	httpd_resp_sendstr_chunk_cr(req, "<table class=\"small\" style=\"width:180px\">" );
	httpd_resp_sendstr_chunk_cr(req, "	<tr>" );

	sprintf(line, "		<td %s><div id=\"play\">playing</div></td>", playing );
	httpd_resp_sendstr_chunk_cr(req, line );

	sprintf(line, "		<td %s><div id=\"pause\">paused</div></td>", paused );
	httpd_resp_sendstr_chunk_cr(req, line);

	sprintf(line, "		<td %s><div id=\"stop\">stopped</div></td>", stopped );
		httpd_resp_sendstr_chunk_cr(req, line);

	sprintf(line, "		<td %s><div id=\"error\">error</div></td>", error );
	httpd_resp_sendstr_chunk_cr(req, line);

	sprintf(line, "		<td %s><div id=\"repeat\">repeat</div></td>", repeat );
	httpd_resp_sendstr_chunk_cr(req, line );

	sprintf(line, "		<td %s><div id=\"shuffle\">shuffle</div></td>", shuffle);
	httpd_resp_sendstr_chunk_cr(req, line );

	httpd_resp_sendstr_chunk_cr(req, "	</tr>" );
	httpd_resp_sendstr_chunk_cr(req, "	<tr></tr>" );
	httpd_resp_sendstr_chunk_cr(req, "</table>" );

	sprintf( line,  "<p id=\"activeTrack\" class=\"radioframe\">%s</p>", ftcSoundBar.pipeline.playList.getActiveTrack() );
	httpd_resp_sendstr_chunk_cr(req, line );

	// httpd_resp_sendstr_chunk_cr(req, "<div style=\"text-align: center;\"><input type=\"range\" value=\"0\" class=\"timeslider\" id=\"FilePos\" onkeypress=\"return false;\"></div>" );

	httpd_resp_sendstr_chunk_cr(req, "<table style=\"width:320px\">" );
	httpd_resp_sendstr_chunk_cr(req, "	<tr>" );

	/* PREVIOUS  */ httpd_resp_send_img( req, "		<td><a onclick=\"previousTrack()\">", IMAGE_PREVIOUS, "</a></td>" );

	/* Play      */ httpd_resp_send_img( req, "		<td><a onclick=\"playTrack()\">", IMAGE_PLAY, "</a></td>" );

	/* STOP */      httpd_resp_send_img( req, "		<td><a onclick=\"stopTrack()\">", IMAGE_STOP, "</a></td>" );

	/* FORWARD  */  httpd_resp_send_img( req, "		<td><a onclick=\"nextTrack()\">", IMAGE_NEXT, "</a></td>" );

	/* REPEAT */    httpd_resp_send_img( req, "		<td><a onclick=\"modeRepeat()\">", IMAGE_REPEAT, "</a></td>" );

	/* SHUFFLE */   httpd_resp_send_img( req, "		<td><a onclick=\"modeShuffle()\">", IMAGE_SHUFFLE, "</a></td>" );

	/* V-Down */    httpd_resp_send_img( req, "		<td><a onclick=\"volume(-10)\">", IMAGE_VOLUME_DOWN, "</a></td>" );

	/* V-Up */      httpd_resp_send_img( req, "		<td><a onclick=\"volume(10)\">", IMAGE_VOLUME_UP, "</a></td>" );

	/* setup */     httpd_resp_send_img( req, "		<td><a href=\"setup\">", IMAGE_SETUP, "</a></td>" );

	httpd_resp_sendstr_chunk_cr(req, "	</tr>" );
	httpd_resp_sendstr_chunk_cr(req, "</table>" );

	httpd_resp_sendstr_chunk_cr(req, "<hr>" );

    httpd_resp_sendstr_chunk_cr(req, "<h3 align=\"center\">Playlist</h3>");

    httpd_resp_sendstr_chunk_cr(req, "<table>" );

    for (int i=0; i < ftcSoundBar.pipeline.playList.getTracks(); i++) {

	  httpd_resp_sendstr_chunk(req, "	<tr>" );

	  sprintf( line, "<td><a onclick=\"play(%d)\" class=\"select\">%03d %s</a></td></tr>", i, i, ftcSoundBar.pipeline.playList.getTrack(i) );
	  httpd_resp_sendstr_chunk(req, line);

	  httpd_resp_sendstr_chunk_cr(req, "</tr>" );
	}

	httpd_resp_sendstr_chunk(req, "</table>" );

	// refresh every 1/4 second
	httpd_resp_sendstr_chunk_cr(req, "<script>" );
	httpd_resp_sendstr_chunk_cr(req, "	var x;" );
	httpd_resp_sendstr_chunk_cr(req, "	x = setInterval(refresh, 250);" );
	httpd_resp_sendstr_chunk_cr(req, "</script>" );

	//httpd_resp_sendstr_chunk_cr( req, "<button type=\"button\" onclick=\"refresh()\">refresh</button>");

	send_footer(req);

	/* Send empty chunk to signal HTTP response completion */
    httpd_resp_sendstr_chunk(req, NULL);

	return ESP_OK;
}

#define TAGAPI "::API"

static esp_err_t track_get_handler(httpd_req_t *req)
{	char tag[20];

	#ifdef DEBUGREST
    	ESP_LOGI( TAGAPI, "GET track" );
	#endif

    httpd_resp_set_type(req, "application/json");

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "tracks", ftcSoundBar.pipeline.playList.getTracks() );
    cJSON_AddNumberToObject(root, "active_track", ftcSoundBar.pipeline.playList.getActiveTrackNr() );

    cJSON_AddNumberToObject(root, "state", (int)ftcSoundBar.pipeline.getState() );

    for (int i=0; i < ftcSoundBar.pipeline.playList.getTracks(); i++) {
          sprintf( tag, "track#%d", i );
          cJSON_AddStringToObject(root, tag, ftcSoundBar.pipeline.playList.getTrack(i) );
    }

    const char *sys_info = cJSON_Print(root);
    httpd_resp_sendstr(req, sys_info);

	#ifdef DEBUGREST
    	ESP_LOGI( TAGAPI, "GET track: %s", sys_info);
	#endif

    free((void *)sys_info);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t active_track_get_handler(httpd_req_t *req)
{
	#ifdef DEBUGREST
		ESP_LOGI( TAGAPI, "GET activeTrack" );
	#endif

    httpd_resp_set_type(req, "application/json");

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "activeTrack", ftcSoundBar.pipeline.playList.getActiveTrack() );
    cJSON_AddNumberToObject(root, "mode", ftcSoundBar.pipeline.getMode() );
    cJSON_AddNumberToObject(root, "state", ftcSoundBar.pipeline.getState() );

    const char *sys_info = cJSON_Print(root);
    httpd_resp_sendstr(req, sys_info);

	#ifdef DEBUGREST
    	ESP_LOGI( TAGAPI, "GET track: %s", sys_info);
	#endif

    free((void *)sys_info);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t volume_get_handler(httpd_req_t *req)
{
	#ifdef DEBUGREST
		ESP_LOGI( TAGAPI, "GET volume" );
	#endif

    httpd_resp_set_type(req, "application/json");
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "volume", ftcSoundBar.pipeline.getVolume() );
    const char *sys_info = cJSON_Print(root);
    httpd_resp_sendstr(req, sys_info);

    #ifdef DEBUGREST
    	ESP_LOGI( TAGAPI, "GET volume: %s", sys_info);
	#endif

    free((void *)sys_info);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t mode_get_handler(httpd_req_t *req)
{
	httpd_resp_set_type(req, "application/json");
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "mode", ftcSoundBar.pipeline.getMode() );
    const char *sys_info = cJSON_Print(root);
    httpd_resp_sendstr(req, sys_info);

	#ifdef DEBUGREST
    	ESP_LOGI( TAGAPI, "GET mode: %s", sys_info);
	#endif

    free((void *)sys_info);
    cJSON_Delete(root);
    return ESP_OK;
}

static char *getBody(httpd_req_t *req)
{
    int total_len = req->content_len;
    int cur_len = 0;
    char *buf = ((http_server_context_t *)(req->user_ctx))->scratch;
    int received = 0;
    if (total_len >= SCRATCH_BUFSIZE) {
        /* Respond with 500 Internal Server Error */
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "content too long");
        return NULL;
    }
    while (cur_len < total_len) {
        received = httpd_req_recv(req, buf + cur_len, total_len);
        if (received <= 0) {
            /* Respond with 500 Internal Server Error */
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to post control value");
            return NULL;
        }
        cur_len += received;
    }
    buf[total_len] = '\0';

    return buf;
}

static esp_err_t play_post_handler(httpd_req_t *req) {

	char *body = getBody(req);
	if (body==NULL) {
		#ifdef DEBUGREST
			ESP_LOGI( TAGAPI, "POST play: <null>");
		#endif
		return ESP_FAIL;
	}

	#ifdef DEBUGREST
		ESP_LOGI( TAGAPI, "POST play: %s", body);
	#endif

    cJSON *root = cJSON_Parse(body);
    if ( root == NULL ) { return ESP_FAIL; }

    cJSON *JSONtrack = cJSON_GetObjectItem(root, "track");
    if ( JSONtrack != NULL ) {
    	int track = JSONtrack->valueint;
    	ftcSoundBar.pipeline.playList.setActiveTrackNr(track);
    }

    ftcSoundBar.pipeline.play( );
    cJSON_Delete(root);
    httpd_resp_sendstr(req, "Post control value successfully");
    return ESP_OK;
}

static esp_err_t config_post_handler(httpd_req_t *req) {

	char *body = getBody(req);
	if (body==NULL) {
		#ifdef DEBUGREST
			ESP_LOGI( TAGAPI, "POST config: <NULL>");
		#endif
		return ESP_FAIL;
	}

	#ifdef DEBUGREST
		ESP_LOGI( TAGAPI, "POST config: %s", body);
	#endif

    cJSON *root = cJSON_Parse(body);
    if ( root == NULL ) { return ESP_FAIL; }

    char *wifi_ssid = cJSON_GetObjectItem(root, "wifi_ssid")->valuestring;
    char *wifi_password = cJSON_GetObjectItem(root, "wifi_password")->valuestring;
    char *txt_ap_mode = cJSON_GetObjectItem(root, "txt_ap_mode")->valuestring;
    strcpy( (char *)ftcSoundBar.WIFI_SSID, wifi_ssid );
    strcpy( (char *)ftcSoundBar.WIFI_PASSWORD, wifi_password);
    ftcSoundBar.TXT_AP_MODE = ( strcmp( txt_ap_mode, "true" ) == 0 );
    ESP_LOGI( TAG, "TXT_AP_MODE: %s",txt_ap_mode);

    ftcSoundBar.writeConfigFile( (char *) CONFIG_FILE );

    cJSON_Delete(root);

    xTaskCreate(&task_reboot, "reboot", 512, NULL, 5, NULL );

    httpd_resp_sendstr(req, "Post control value successfully");
    return ESP_OK;
}

static esp_err_t ota_post_handler(httpd_req_t *req) {

	char *body = getBody(req);
	if (body==NULL) {
		#ifdef DEBUGREST
			ESP_LOGI( TAGAPI, "POST ota: <NULL>");
		#endif
		return ESP_FAIL;
	}

	#ifdef DEBUGREST
		ESP_LOGI( TAGAPI, "POST ota: %s", body);
	#endif

    cJSON *root = cJSON_Parse(body);
    if ( root == NULL ) { return ESP_FAIL; }
    cJSON_Delete(root);

    xTaskCreate(&task_ota, "ota", 8192, NULL, 5, NULL );

    httpd_resp_sendstr(req, "Post control value successfully");
    return ESP_OK;
}


static esp_err_t volume_post_handler(httpd_req_t *req)
{
	char *body = getBody(req);
	if (body==NULL) {
		#ifdef DEBUGREST
			ESP_LOGI( TAGAPI, "POST volumne: <NULL>");
		#endif
		return ESP_FAIL;
	}

	#ifdef DEBUGREST
		ESP_LOGI( TAGAPI, "POST volume: %s", body);
	#endif

    cJSON *root = cJSON_Parse(body);
    cJSON *JSONvolume = cJSON_GetObjectItem(root, "volume");
    cJSON *JSONrelvolume = cJSON_GetObjectItem(root, "relvolume");

    if ( JSONvolume != NULL ) {
    	int vol = JSONvolume->valueint;
    	ftcSoundBar.pipeline.setVolume( vol );
    } else if ( JSONrelvolume != NULL ) {
    	int relvol = JSONrelvolume->valueint;
    	if ( relvol > 0 ) {
    		ftcSoundBar.pipeline.setVolume( INC_VOLUME );
    	} else {
    		ftcSoundBar.pipeline.setVolume( DEC_VOLUME );
    	}

    }
    cJSON_Delete(root);
    httpd_resp_sendstr(req, "Post control value successfully");
    return ESP_OK;
}

static esp_err_t previous_post_handler(httpd_req_t *req)
{
	char *body = getBody(req);
	if (body==NULL) {
		#ifdef DEBUGREST
			ESP_LOGI( TAGAPI, "POST previous: <NULL>");
		#endif
		return ESP_FAIL;
	}

	#ifdef DEBUGREST
		ESP_LOGI( TAGAPI, "POST previous: %s", body);
	#endif

    ftcSoundBar.pipeline.playList.prevTrack();
    ftcSoundBar.pipeline.play( );

    httpd_resp_sendstr(req, "Post control value successfully");
    return ESP_OK;
}

static esp_err_t next_post_handler(httpd_req_t *req)
{
	char *body = getBody(req);
	if (body==NULL) {
		#ifdef DEBUGREST
			ESP_LOGI( TAGAPI, "POST next: <NULL>");
		#endif
		return ESP_FAIL;
	}

	#ifdef DEBUGREST
		ESP_LOGI( TAGAPI, "POST next: %s", body);
	#endif

	ftcSoundBar.pipeline.playList.nextTrack();
    ftcSoundBar.pipeline.play( );

    httpd_resp_sendstr(req, "Post control value successfully");
    return ESP_OK;
}

static esp_err_t stop_post_handler(httpd_req_t *req)
{
	char *body = getBody(req);
	if (body==NULL) {
		#ifdef DEBUGREST
			ESP_LOGI( TAGAPI, "POST stop: <NULL>");
		#endif
		return ESP_FAIL;
	}

	#ifdef DEBUGREST
		ESP_LOGI( TAGAPI, "POST stop: %s", body);
	#endif

	ftcSoundBar.pipeline.stop();
	ftcSoundBar.pipeline.setMode( MODE_SINGLE_TRACK );

    httpd_resp_sendstr(req, "Post control value successfully");
    return ESP_OK;
}

static esp_err_t mode_post_handler(httpd_req_t *req)
{
	char *body = getBody(req);
	if (body==NULL) {
		#ifdef DEBUGREST
			ESP_LOGI( TAGAPI, "POST mode: <NULL>");
		#endif
		return ESP_FAIL;
	}

	#ifdef DEBUGREST
		ESP_LOGI( TAGAPI, "POST mode: %s", body);
	#endif

    cJSON *root = cJSON_Parse(body);
    int mode = cJSON_GetObjectItem(root, "mode")->valueint;
    ftcSoundBar.pipeline.setMode( (play_mode_t) mode );
    if ( !ftcSoundBar.pipeline.isPlaying() ) {
    	ftcSoundBar.pipeline.play( );
    }

    cJSON_Delete(root);
    httpd_resp_sendstr(req, "Post control value successfully");
    return ESP_OK;
}

static esp_err_t pause_post_handler(httpd_req_t *req)
{
	char *body = getBody(req);
	if (body==NULL) {
		#ifdef DEBUGREST
			ESP_LOGI( TAGAPI, "POST pause: <NULL>");
		#endif
		return ESP_FAIL;
	}

	#ifdef DEBUGREST
		ESP_LOGI( TAGAPI, "POST pause: %s", body);
	#endif

    ftcSoundBar.pipeline.pause();

    httpd_resp_sendstr(req, "Post control value successfully");
    return ESP_OK;
}

static esp_err_t resume_post_handler(httpd_req_t *req)
{
	char *body = getBody(req);
	if (body==NULL) {
        #ifdef DEBUGREST
			ESP_LOGI( TAGAPI, "POST resume: <NULL>");
		#endif
		return ESP_FAIL;
	}

    #ifdef DEBUGREST
		ESP_LOGI( TAGAPI, "POST resume: %s", body);
	#endif

    ftcSoundBar.pipeline.resume();

    httpd_resp_sendstr(req, "Post control value successfully");
    return ESP_OK;
}

#define TAGWEB "WEBSERVER"

/* Function to start the web server */
esp_err_t start_web_server( const char *base_path )
{

    http_server_context_t *http_context = (http_server_context_t*)calloc(1, sizeof(http_server_context_t));
    strlcpy(http_context->base_path, base_path, sizeof(http_context->base_path));

    httpd_handle_t server = NULL;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.max_uri_handlers = 20;
    config.stack_size = 20480;

    ESP_LOGI(TAGWEB, "Starting HTTP Server");
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAGWEB, "Failed to start web server!");
        return ESP_FAIL;
    }

    // /
    httpd_uri_t root_html = { .uri = "/", .method = HTTP_GET, .handler   = root_html_get_handler , .user_ctx = NULL };
    httpd_register_uri_handler(server, &root_html);

    // setup
    httpd_uri_t setup_html = { .uri = "/setup", .method = HTTP_GET, .handler = setup_html_get_handler, .user_ctx = NULL };
    httpd_register_uri_handler(server, &setup_html);

    // styles.css
    httpd_uri_t styles_css = { .uri = "/styles.css", .method = HTTP_GET, .handler = styles_css_get_handler, .user_ctx = NULL };
    httpd_register_uri_handler(server, &styles_css);

    // favicon
    httpd_uri_t favico = { .uri = "/favicon.ico", .method = HTTP_GET, .handler = favico_get_handler, .user_ctx = NULL };
    httpd_register_uri_handler(server, &favico);

    // API
    httpd_uri_t track_get_uri = {.uri = "/api/track", .method = HTTP_GET, .handler = track_get_handler, .user_ctx = NULL };
    httpd_register_uri_handler(server, &track_get_uri);

    // active_track
    httpd_uri_t active_track_html = { .uri = "/api/activeTrack", .method = HTTP_GET, .handler = active_track_get_handler, .user_ctx = NULL };
    httpd_register_uri_handler(server, &active_track_html);

    httpd_uri_t play_post_uri = { .uri = "/api/track/play", .method = HTTP_POST, .handler = play_post_handler, .user_ctx = http_context };
    httpd_register_uri_handler(server, &play_post_uri);

    httpd_uri_t config_post_uri = { .uri = "/api/config",.method = HTTP_POST, .handler = config_post_handler, .user_ctx = http_context };
    httpd_register_uri_handler(server, &config_post_uri);

    httpd_uri_t ota_post_uri = { .uri = "/api/ota", .method = HTTP_POST, .handler = ota_post_handler, .user_ctx = http_context };
    httpd_register_uri_handler(server, &ota_post_uri);

    httpd_uri_t volume_post_uri = { .uri = "/api/volume", .method = HTTP_POST, .handler = volume_post_handler, .user_ctx = http_context };
    httpd_register_uri_handler(server, &volume_post_uri);

    httpd_uri_t volume_get_uri = { .uri = "/api/volume", .method = HTTP_GET, .handler = volume_get_handler, .user_ctx = http_context };
    httpd_register_uri_handler(server, &volume_get_uri);

    httpd_uri_t previous_post_uri = { .uri = "/api/track/previous", .method = HTTP_POST, .handler = previous_post_handler, .user_ctx = http_context };
    httpd_register_uri_handler(server, &previous_post_uri);

    httpd_uri_t next_post_uri = { .uri = "/api/track/next", .method = HTTP_POST, .handler = next_post_handler, .user_ctx = http_context };
    httpd_register_uri_handler(server, &next_post_uri);

    httpd_uri_t stop_post_uri = { .uri = "/api/track/stop", .method = HTTP_POST, .handler = stop_post_handler, .user_ctx = http_context };
    httpd_register_uri_handler(server, &stop_post_uri);

    httpd_uri_t mode_post_uri = { .uri = "/api/mode", .method = HTTP_POST, .handler = mode_post_handler, .user_ctx = http_context };
    httpd_register_uri_handler(server, &mode_post_uri);

    httpd_uri_t mode_get_uri = { .uri = "/api/mode", .method = HTTP_GET, .handler = mode_get_handler, .user_ctx = http_context };
    httpd_register_uri_handler(server, &mode_get_uri);

    httpd_uri_t pause_post_uri = { .uri = "/api/track/pause", .method = HTTP_POST, .handler = pause_post_handler, .user_ctx = http_context };
    httpd_register_uri_handler(server, &pause_post_uri);

    httpd_uri_t resume_post_uri = { .uri = "/api/track/resume", .method = HTTP_POST, .handler = resume_post_handler, .user_ctx = http_context };
    httpd_register_uri_handler(server, &resume_post_uri);

    return ESP_OK;
}

/******************************************************************
 *
 * wifi
 *
 ******************************************************************/

#define TAGWIFI "WIFI"

esp_err_t wifi_event_handler(void *ctx, system_event_t *event)
{
	ESP_LOGI(TAGWIFI, "wifi_event_handler");

    switch(event->event_id) {
    case SYSTEM_EVENT_STA_START:
        ESP_LOGI(TAGWIFI, "SYSTEM_EVENT_STA_START");
        ESP_ERROR_CHECK( esp_wifi_connect() );
        ESP_ERROR_CHECK( tcpip_adapter_set_hostname(TCPIP_ADAPTER_IF_STA, ftcSoundBar.HOSTNAME) );

    	// set static IP?
        if ( ftcSoundBar.TXT_AP_MODE) {
        	// DHCP off
        	ESP_ERROR_CHECK( tcpip_adapter_dhcpc_stop(TCPIP_ADAPTER_IF_STA));

        	// set 192.168.8.1
        	tcpip_adapter_ip_info_t myip;
        	IP4_ADDR( &myip.gw, 192,168,8,1 );
        	IP4_ADDR( &myip.ip, 192,168,8,100 );
        	IP4_ADDR( &myip.netmask, 255,255,255,0);

        	ESP_ERROR_CHECK( tcpip_adapter_set_ip_info(TCPIP_ADAPTER_IF_STA, &myip));
        }

        break;
    case SYSTEM_EVENT_STA_GOT_IP:
    	ESP_LOGI(TAGWIFI, "SYSTEM_EVENT_STA_GOT_IP");
        ESP_LOGI(TAGWIFI, "Got IP: '%s'",
                ip4addr_ntoa((ip4_addr_t*) &event->event_info.got_ip.ip_info.ip));
        xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
        break;
    case SYSTEM_EVENT_STA_DISCONNECTED:
        ESP_LOGI(TAGWIFI, "SYSTEM_EVENT_STA_DISCONNECTED");
        ESP_ERROR_CHECK(esp_wifi_connect());
        break;
    default:
        break;
    }

    return ESP_OK;
}

void init_wifi( void ) {

    wifi_event_group = xEventGroupCreate();
    tcpip_adapter_init();

    ESP_ERROR_CHECK( esp_event_loop_init(wifi_event_handler, NULL) );
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK( esp_wifi_init(&cfg) );
    ESP_ERROR_CHECK( esp_wifi_set_storage(WIFI_STORAGE_RAM) );
    ESP_ERROR_CHECK( esp_wifi_set_mode( WIFI_MODE_STA ) );
    wifi_config_t sta_config = {};
    memcpy( sta_config.sta.ssid, ftcSoundBar.WIFI_SSID, 32);
    memcpy( sta_config.sta.password, ftcSoundBar.WIFI_PASSWORD, 64);
    ESP_ERROR_CHECK( esp_wifi_set_config(WIFI_IF_STA, &sta_config) );
    ESP_ERROR_CHECK( esp_wifi_start() );
    ESP_ERROR_CHECK( esp_wifi_connect() );

    xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT,
                        false, true, portMAX_DELAY);
    ESP_LOGI(TAGWIFI, "wifi connected!");
    if ( ftcSoundBar.xBlinky != NULL ) { vTaskDelete( ftcSoundBar.xBlinky ); }
	gpio_set_level((gpio_num_t)BLINK_GPIO, 1);

	mdns_init();
	mdns_hostname_set( ftcSoundBar.HOSTNAME );

}

/******************************************************************
 *
 * keyboard
 *
 ******************************************************************/

#define TAGKEY "KEYBOARD"

static esp_err_t input_key_service_cb(periph_service_handle_t handle, periph_service_event_t *evt, void *ctx)
{
    /* Handle touch pad events
           to start, pause, resume, finish current song and adjust volume
        */

    if (evt->type == INPUT_KEY_SERVICE_ACTION_CLICK_RELEASE) {
        ESP_LOGI(TAGKEY, "[ * ] input key id is %d", (int)evt->data);
        switch ((int)evt->data) {
            case INPUT_KEY_USER_ID_PLAY: {
                ESP_LOGI(TAGKEY, "[ * ] [Play] input key event");
                audio_element_state_t el_state = ftcSoundBar.pipeline.getState();
                switch (el_state) {
                    case AEL_STATE_INIT :
                        ESP_LOGI(TAGKEY, "[ * ] Starting audio pipeline");
                        ftcSoundBar.pipeline.play();
                        break;
                    case AEL_STATE_RUNNING :
                        ESP_LOGI(TAGKEY, "[ * ] Pausing audio pipeline");
                        ftcSoundBar.pipeline.pause();
                        break;
                    case AEL_STATE_PAUSED :
                        ESP_LOGI(TAGKEY, "[ * ] Resuming audio pipeline");
                        ftcSoundBar.pipeline.resume();
                        break;
                    default :
                        ESP_LOGI(TAGKEY, "[ * ] Not supported state %d", el_state);
                }
                break; }
            case INPUT_KEY_USER_ID_SET:
                ESP_LOGI(TAGKEY, "[ * ] [Set] input key event");
                ESP_LOGI(TAGKEY, "[ * ] Stopped, advancing to the next song");
                ftcSoundBar.pipeline.playList.nextTrack();
                ftcSoundBar.pipeline.play( );
                break;
            case INPUT_KEY_USER_ID_VOLUP:
                ESP_LOGI(TAGKEY, "[ * ] [Vol+] input key event");
                ftcSoundBar.pipeline.setVolume( INC_VOLUME );
                break;
            case INPUT_KEY_USER_ID_VOLDOWN:
                ESP_LOGI(TAGKEY, "[ * ] [Vol-] input key event");
                ftcSoundBar.pipeline.setVolume( DEC_VOLUME );
                break;
        }
    }

    return ESP_OK;
}

void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_INFO);


    ESP_LOGI(TAG, "ftMusicBox startup" );
    ESP_LOGI(TAG, "(C) 2020 Idee: Oliver Schmiel, Programmierung: Christian Bergschneider, Stefan Fuss" );

    xTaskCreate(&task_blinky, "blinky", 512, NULL, 5, &(ftcSoundBar.xBlinky) );

    nvs_flash_init();

    ESP_LOGI(TAG, "[1.0] Initialize peripherals management");
    esp_periph_config_t periph_cfg = DEFAULT_ESP_PERIPH_SET_CONFIG();
    esp_periph_set_handle_t set = esp_periph_set_init(&periph_cfg);

    ESP_LOGI(TAG, "[1.1] Initialize and start peripherals");
    audio_board_key_init(set);
    audio_board_sdcard_init(set, SD_MODE_1_LINE);

    ESP_LOGI(TAG, "[1.2] Set up a sdcard playlist and scan sdcard music save to it");
    ftcSoundBar.pipeline.playList.readDir( "/sdcard" );

    ESP_LOGI(TAG, "[1.3] read config file");
    ftcSoundBar.readConfigFile( (char *) CONFIG_FILE );

    ESP_LOGI(TAG, "[2.0] Initialize wifi" );
    init_wifi();

    ESP_LOGI(TAG, "[3.0] Start codec chip");
    ftcSoundBar.pipeline.StartCodec();
    ftcSoundBar.pipeline.build( FILETYPE_MP3 );

    ESP_LOGI(TAG, "[4.0] Create and start input key service");
    input_key_service_info_t input_key_info[] = INPUT_KEY_DEFAULT_INFO();
    input_key_service_cfg_t input_cfg = _INPUT_KEY_SERVICE_DEFAULT_CONFIG();
    input_cfg.handle = set;
    periph_service_handle_t input_ser = input_key_service_create(&input_cfg);
    input_key_service_add_key(input_ser, input_key_info, INPUT_KEY_NUM);
    periph_service_set_callback(input_ser, input_key_service_cb, (void *)ftcSoundBar.pipeline.getBoardHandle());

    ESP_LOGI(TAG, "[5.0] Press the keys to control music player:");
    ESP_LOGI(TAG, "      [Play] to start, pause and resume, [Set] next song.");
    ESP_LOGI(TAG, "      [Vol-] or [Vol+] to adjust volume.");

    ESP_LOGI(TAG, "[6.0] Start Web Server");
    ESP_ERROR_CHECK( start_web_server( "localhost" ) );

    ESP_LOGI(TAG, "[7.0] Everything started");

    ftcSoundBar.pipeline.setVolume(15);

   	ftcSoundBar.pipeline.eventLoop();

}