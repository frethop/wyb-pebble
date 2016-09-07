//-----------------------------------------------------------------------------------
//
//  Source code to Pebble "Wear Your Barcode" app
//
//  Mike Jipping, June 2015

#include <pebble.h>

#define VERSION_STRING "WYB 3.0 (06/10/15)"
#define WATCHAPP_VERSION_NUMBER 30

#define KEY_MESSAGE_TYPE_OUT 200
#define KEY_MESSAGE_TYPE_DEBUG 201
#define KEY_MESSAGE_TYPE_VERSION 202

#define NAME_BUFFER_SIZE 10
#define BYTES_PER_ROW 16
#define BYTES_PER_SCREEN (BYTES_PER_ROW*168)

// These are the commands that can be sent to the phone.
enum {
  CMD_KEY = 0x0, // TUPLE_INTEGER
  REQUEST_BARCODE_LIST_LENGTH = 0x10,
  SEND_BARCODE_NAMES = 0x11,
  BARCODE_NAME = 0x12,
  SEND_BARCODE_NAME = 0x13,
  SEND_BARCODE = 0x14,
  BARCODE_IMAGE_BYTES = 0x15,
  BARCODE_IMAGE = 0x16,
  BARCODE_IMAGE_DONE = 0x17,
  BARCODE_NEXT_CHUNK = 0x18,
  BARCODE_FORMAT = 0x19,
  REREAD_BARCODE_LIST = 0x21,
  DISPLAY_BARCODE = 0x22,
  BARCODE_ERROR = 0xFF,
};

// These are the states the app can be in while waiting for data from the phone.
enum {
   STATE_NONE = 0x00,
   STATE_WAITING_BARCODE_LIST_LENGTH = 0x01,
   STATE_WAITING_BARCODE_NAMES = 0x02,
   STATE_RECEIVING_INITIAL_BARCODE_NAMES = 0x03,
   STATE_RECEIVING_SINGLE_NAME = 0x04,
   STATE_RECEIVING_BARCODE = 0x05,
   STATE_ERROR = 0x06,
};

// Declarations for UI
static Window *window;
static Window *barcode_window;
static MenuLayer *mainMenu;
static BitmapLayer *barcodeImageLayer;
static TextLayer *pleaseWait;

static int state;
static int barcodeNameCount, numBarcodes;
static char *nameBuffer[NAME_BUFFER_SIZE];
static char *formatBuffer[NAME_BUFFER_SIZE];
static int nameBufferPtr = 0;
static GBitmap *barcodeImageBitmap;
static uint8_t *barcodeImageData; //[BYTES_PER_SCREEN];
static int firstRow;
static int justSelected;
static int autoDisplay=0;
static bool transferDone;
static bool error;

char* msg;

static void mainMenu_select_long_click(MenuLayer *menu_layer, MenuIndex *cell_index, void *data);

int red, green, blue;

//---------------------------------------------------------------------------------
// AppMessage methods

// Function used to send request/data pair to the phone.  
static void send_request(uint8_t req, uint16_t data) {

  Tuplet value = TupletInteger(req, data);

  DictionaryIterator *iter;
  app_message_outbox_begin(&iter);

  if (iter == NULL) return;

  dict_write_tuplet(iter, &value);
  dict_write_end(iter);

  app_message_outbox_send();
}

// This is a callback for messages received from the phone.
// It is heavily dependent on the state of the data exchange with the phone.
static void app_received_msg(DictionaryIterator* received, void* context) {
  Tuple *tuple;
  int value=-1;
  int offset;
  uint16_t rowNumber;
  uint8_t bytesPerRow;

  // First, check if we are getting an ERROR response from the phone.
  error = false;
  tuple = dict_find(received, BARCODE_ERROR);
  if (tuple != NULL) {
	 msg = (char *)malloc(tuple->length+8);
	 strcpy(msg, "ERROR: ");
	 strcat(msg, tuple->value->cstring);
     state = STATE_ERROR;
  }

  // Check the state of te data exchange.
  switch (state) {
      
     // If state == STATE_NONE, we are started a new conversation.
     // The phone is sending the app a message.
     case STATE_NONE:
        APP_LOG(APP_LOG_LEVEL_DEBUG, "state is STATE_NONE");
        tuple = dict_find(received, CMD_KEY);
        if (tuple != NULL) {
            value = tuple->value->uint8;
            if (value == REREAD_BARCODE_LIST) {
                APP_LOG(APP_LOG_LEVEL_DEBUG, "Received REREAD_BARCODE_LIST" );
                mainMenu_select_long_click(NULL, NULL, NULL);  // same action as long click
            } else {
                APP_LOG(APP_LOG_LEVEL_DEBUG, "Received CMD_KEY, but not something we know.");
            }
        } else {
            APP_LOG(APP_LOG_LEVEL_DEBUG, "Received SOMETHING not CMD_KEY");
            tuple = dict_find(received, DISPLAY_BARCODE);
            if (tuple != NULL) {
                value = tuple->value->uint8;
                if (value < numBarcodes) {
                    window_stack_push(barcode_window, true /* Animated */);
		            state = STATE_RECEIVING_BARCODE;
		            send_request(SEND_BARCODE, value);
		            justSelected = value;
                }
            } else {
                APP_LOG(APP_LOG_LEVEL_DEBUG, "Received something not recognized.");
            }
        }
        break;
      
     // If state == STATE_ERROR, we received a BARCODE_ERROR message from the phone
     // and constructed an error message.  Handle it here wit a vibration.
     case STATE_ERROR:
        error = true;
        vibes_long_pulse();
        layer_mark_dirty(bitmap_layer_get_layer(barcodeImageLayer));
        free(msg);
        state = STATE_NONE;
        break;

     // If state == STATE_WAITING_BARCODE_LIST_LENGTH, we are waiting on data from the
     // phone indicating the length of the barcode list on the phone.  
     //    value of 0 == no barcodes
     //    value of 255 == the phone is initiating a barcode display
     //    other values == length of the barcode list
     case STATE_WAITING_BARCODE_LIST_LENGTH:
        APP_LOG(APP_LOG_LEVEL_DEBUG, "state is STATE_WAITING_BARCODE_LIST_LENGTH");
        tuple = dict_find(received, REQUEST_BARCODE_LIST_LENGTH);
        if (tuple != NULL) {
           value = tuple->value->uint8;
           // empty list
           if (value == 0) {
        	   state = STATE_NONE;
               menu_layer_reload_data(mainMenu);
           // phone wants to display a specific barcode
           } else if (value == 255) {
        	   send_request(SEND_BARCODE, 255);
        	   state = STATE_RECEIVING_BARCODE;
        	   window_stack_push(barcode_window, true /* Animated */);
        	   autoDisplay = 1;
           // length is real
           } else {
		      send_request(SEND_BARCODE_NAME, 0);
			  state = STATE_RECEIVING_INITIAL_BARCODE_NAMES;
			  numBarcodes = value;
			  barcodeNameCount = 0;
          }
        }
        break;
      
     // If state == STATE_RECEIVING_INITIAL_BARCODE_NAMES, we are waiting for the list of 
     // barcode names from the phone.  We need to accept them and work the into the barcode
     // list by reloading the menu data.
     case STATE_RECEIVING_INITIAL_BARCODE_NAMES:
        APP_LOG(APP_LOG_LEVEL_DEBUG, "state is STATE_RECEIVING_INITIAL_BARCODE_NAMES");
        tuple = dict_find(received, BARCODE_NAME);
        if (tuple) {
			nameBuffer[nameBufferPtr] = (char *) malloc(tuple->length);
			strcpy(nameBuffer[nameBufferPtr], tuple->value->cstring);
			tuple = dict_find(received, BARCODE_FORMAT);
			formatBuffer[nameBufferPtr] = (char *) malloc(tuple->length);
			strcpy(formatBuffer[nameBufferPtr], tuple->value->cstring);
			nameBufferPtr++;
			barcodeNameCount++;

			if (nameBufferPtr == numBarcodes || nameBufferPtr == NAME_BUFFER_SIZE) {
				state = STATE_NONE;
                menu_layer_reload_data(mainMenu);
			} else {
				send_request(SEND_BARCODE_NAME, nameBufferPtr);
			}
        }
	    break;

     // If state == STATE_WAITING_BARCODE_NAMES, we are waiting for MORE barcode names.
     // we are getting more names.  We add them to the menu list.
     case STATE_WAITING_BARCODE_NAMES:
        APP_LOG(APP_LOG_LEVEL_DEBUG, "state is STATE_WAITING_BARCODE_NAMES");
        tuple = dict_find(received, BARCODE_NAME);
        if (tuple != NULL) {
            nameBuffer[nameBufferPtr] = (char *) malloc(tuple->length);
    		strcpy(nameBuffer[nameBufferPtr], tuple->value->cstring);
            tuple = dict_find(received, BARCODE_FORMAT);
    	    formatBuffer[nameBufferPtr] = (char *) malloc(tuple->length);
    		strcpy(formatBuffer[nameBufferPtr], tuple->value->cstring);
    	    nameBufferPtr = nameBufferPtr % NAME_BUFFER_SIZE;
    	    barcodeNameCount++;
            menu_layer_reload_data(mainMenu);
        } else {
            mainMenu_select_long_click(NULL, NULL, NULL);  // same action as long click
            state = STATE_NONE;
        }
        if (barcodeNameCount == numBarcodes)
            state = STATE_NONE;
	    break;

     // If state == STATE_RECEIVING_BARCODE, we are received barcode data from the phone.  
     // The first packet is the number of bytes in the image.  The remaining packets from the 
     // phone are in the form <row number, byte data for row>
     case STATE_RECEIVING_BARCODE:
        APP_LOG(APP_LOG_LEVEL_DEBUG, "state is STATE_RECEIVING_BARCODE");
         tuple = dict_find(received, BARCODE_IMAGE_BYTES);
         // First packet with data.  Ask for that first row.
   	     if (tuple) {
   	    	 firstRow = tuple->value->uint16;
   	    	 send_request(BARCODE_NEXT_CHUNK, firstRow);
             transferDone = false;
             layer_mark_dirty(bitmap_layer_get_layer(barcodeImageLayer));
   	    	 break;
         // Other rows, collect the data, and ask for next row.... unless we are done.
   	     } else {
   	         tuple = dict_find(received, BARCODE_IMAGE);
   	         if (tuple) {
   	        	 rowNumber = (tuple->value->data[0]+(tuple->value->data[1] << 8));
   	        	 bytesPerRow = tuple->value->data[2];
				 offset = rowNumber*bytesPerRow;
                 APP_LOG(APP_LOG_LEVEL_DEBUG, ">> Offset = %d, rowNumber = %d, bytesPerRow = %d\n", offset, rowNumber, bytesPerRow);
				 if (offset < 2640)
                     memcpy(barcodeImageData+offset, &(tuple->value->data[3]), tuple->length-3);
	        	 //layer_mark_dirty(bitmap_layer_get_layer(barcodeImageLayer));
	        	 send_request(BARCODE_NEXT_CHUNK, rowNumber);
   	         } else {
   	        	tuple = dict_find(received, BARCODE_IMAGE_DONE);
   	        	if (tuple) {
      	        	APP_LOG(APP_LOG_LEVEL_DEBUG, "Got BARCODE_IMAGE_DONE");
                    transferDone = true;
   	        		layer_mark_dirty(bitmap_layer_get_layer(barcodeImageLayer));
   	   	        	vibes_short_pulse();
   	        	}
        		state = STATE_NONE;
   	         }
   	     }
    	 break;

     // Oops.
     default:
        APP_LOG(APP_LOG_LEVEL_DEBUG, "An ERROR has occurred in receiving");
        break;
  }
}

//---------------------------------------------------------------------------------
// Menu event handlers

static uint16_t mainMenu_get_num_sections(MenuLayer *menu_layer, void *data)
{ // Always 1 sections
	return 1;
}

static uint16_t mainMenu_get_num_rows_in_section(MenuLayer *menu_layer, uint16_t section_index, void *data)
{ 
    // each section has 1 line (no barcodes) or lines for each barcode.
	return numBarcodes==0?1:numBarcodes;
}

static int16_t mainMenu_get_cell_height(MenuLayer *menu_layer, MenuIndex *cell_index, void *data)
{
   return 30;
}

static int16_t mainMenu_get_header_height(MenuLayer *menu_layer, uint16_t section_index, void *data)
{
  return (const int16_t)35;
}

// Select button in the menu will start the barcode data fetch and display, unless there are 
// no barcodes.
static void mainMenu_select_click(MenuLayer *menu_layer, MenuIndex *cell_index, void *data)
{
	if (barcodeNameCount == 0) {
		state = STATE_NONE;
	} else {
		window_stack_push(barcode_window, true /* Animated */);

		state = STATE_RECEIVING_BARCODE;
		send_request(SEND_BARCODE, cell_index->row);
		justSelected = cell_index->row;
	}
}

// A long click is a menu reload.
static void mainMenu_select_long_click(MenuLayer *menu_layer, MenuIndex *cell_index, void *data)
{
	  justSelected = -1;
	  nameBufferPtr = 0;
	  state = STATE_WAITING_BARCODE_LIST_LENGTH;
	  send_request(REQUEST_BARCODE_LIST_LENGTH, WATCHAPP_VERSION_NUMBER);
}

// The menu header is a big "Barcodes" sign.
static void mainMenu_draw_header(GContext* ctx, const Layer *cell_layer, uint16_t section_index, void *data)
{
    GRect bounds = layer_get_frame(cell_layer);
    red = 100; green = 0; blue = 0;
    graphics_context_set_fill_color(ctx, GColorFromRGB(red, green, blue));
    graphics_fill_rect(ctx, bounds, 0, GCornerNone);
    graphics_context_set_text_color(ctx, GColorWhite);
    graphics_draw_text(ctx,
    		"Barcodes",
    		fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD),
    		bounds,
    		GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
}

// Draw each row in the menu.  If the number of barcodes == 0, display the NO BARCODES message.
// If the number of barcodes > 0, display the name of the barcode at the position in the menu.
// AND start a new download of names if we need to.
static void mainMenu_draw_row(GContext *ctx, const Layer *cell_layer, MenuIndex *cell_index, void *data)
{
	int barcodeNameNumber = cell_index->row;
    int selection = (menu_layer_get_selected_index(mainMenu)) . row;

    if (barcodeNameNumber == selection) {
       graphics_context_set_text_color(ctx, GColorWhite);
    } else {
       graphics_context_set_text_color(ctx, GColorBlack);
    }

    red = 100; green = 0; blue = 0;
    GRect bounds = layer_get_bounds(cell_layer);
    if (barcodeNameCount == 0) {
	   graphics_draw_text(ctx,
			   "No Barcodes Listed",
			   fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD),
			   bounds,
			   GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
   } else {
        menu_cell_basic_draw(ctx, cell_layer, 
                             nameBuffer[barcodeNameNumber % NAME_BUFFER_SIZE], 
                             formatBuffer[barcodeNameNumber % NAME_BUFFER_SIZE], 
                             NULL);
		if (barcodeNameNumber < numBarcodes && barcodeNameNumber+1 > barcodeNameCount) {
			send_request(SEND_BARCODE_NAME, barcodeNameNumber+1);
			state = STATE_WAITING_BARCODE_NAMES;
		}
   }
}

// Draw the bitmap!  Update function for the bitmap layer.
static void bitmap_layer_update_callback(Layer *layer, GContext *ctx) {
    
    // Hide the "Please Wait" layer and expose the bitmap layer
    layer_set_hidden(bitmap_layer_get_layer(barcodeImageLayer), false);
    layer_set_hidden(text_layer_get_layer(pleaseWait), true);
    
    if (error) {
        text_layer_set_text(pleaseWait, msg);
        return;
    }

    // We we are done with the transfer of bitmap data from the phone....display it!
    // We need to destroy the old bitmap and create a new one, set its data, then draw using "assign" compositing op.
    if (transferDone) {
        gbitmap_destroy(barcodeImageBitmap);
        barcodeImageBitmap = gbitmap_create_blank(GSize(BYTES_PER_ROW*8, 168 /*bounds.size.h*/), GBitmapFormat1Bit);
        if (barcodeImageBitmap == NULL) {
            APP_LOG(APP_LOG_LEVEL_DEBUG, "barcodeImageBitmap = NULL");
            return;
        }
        gbitmap_set_data(barcodeImageBitmap, barcodeImageData, GBitmapFormat1Bit, BYTES_PER_ROW, false);

        GRect destination = gbitmap_get_bounds(barcodeImageBitmap);

        graphics_context_set_compositing_mode(ctx, GCompOpAssign);
        graphics_draw_bitmap_in_rect(ctx, barcodeImageBitmap, GRect(8,0, destination.size.w, destination.size.h));
    } else {
        // NOT DONE!
        layer_set_hidden(bitmap_layer_get_layer(barcodeImageLayer), true);
        layer_set_hidden(text_layer_get_layer(pleaseWait), false);
    }
}

//---------------------------------------------------------------------------------
// Window loaders and unloaders.  Use them for setup.

static void barcode_window_load(Window *window) {
	APP_LOG(APP_LOG_LEVEL_DEBUG, "calling barcode_window_load");
	Layer *window_layer = window_get_root_layer(window);
	GRect bounds = layer_get_frame(window_layer);
    
    barcodeImageData = (uint8_t *)malloc(BYTES_PER_SCREEN);
	memset(barcodeImageData, -1, BYTES_PER_SCREEN);
    
    pleaseWait = text_layer_create(GRect(0,65,bounds.size.w,140));
    text_layer_set_text_alignment(pleaseWait, GTextAlignmentCenter);
    text_layer_set_font(pleaseWait, fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD));
    text_layer_set_text(pleaseWait, "Please wait...");
    layer_add_child(window_layer, text_layer_get_layer(pleaseWait));
    layer_set_hidden(text_layer_get_layer(pleaseWait), true);
    
    barcodeImageLayer = bitmap_layer_create(GRect(0,0,bounds.size.w,bounds.size.h));
	layer_set_update_proc(bitmap_layer_get_layer(barcodeImageLayer), bitmap_layer_update_callback);
	layer_add_child(window_layer, bitmap_layer_get_layer(barcodeImageLayer));
}

static void barcode_window_unload(Window *window) {
	APP_LOG(APP_LOG_LEVEL_DEBUG, "calling barcode_window_unload");
 	bitmap_layer_destroy(barcodeImageLayer);
	free(barcodeImageData);

	if (autoDisplay) {
		autoDisplay=0;
		justSelected = -1;
		nameBufferPtr = 0;
		state = STATE_WAITING_BARCODE_LIST_LENGTH;
		send_request(REQUEST_BARCODE_LIST_LENGTH, WATCHAPP_VERSION_NUMBER);
	}
}

static void window_load(Window *window) {
	APP_LOG(APP_LOG_LEVEL_DEBUG, "calling window_load");
	Layer *window_layer = window_get_root_layer(window);
	GRect bounds = layer_get_frame(window_layer);

  mainMenu = menu_layer_create(bounds); 

  // Set all of our callbacks.
  menu_layer_set_callbacks(mainMenu, NULL, (MenuLayerCallbacks){
	  .get_num_sections = mainMenu_get_num_sections,
	  .get_num_rows = mainMenu_get_num_rows_in_section,
	  .get_header_height = mainMenu_get_header_height,
	  .draw_header = mainMenu_draw_header,
	  .draw_row = mainMenu_draw_row,
	  .select_click = mainMenu_select_click,
	  .select_long_click = mainMenu_select_long_click,
  });

  // Bind the menu layer's click config provider to the window for interactivity
  menu_layer_set_click_config_onto_window(mainMenu, window);

  // Add it to the window for display
  layer_add_child(window_layer, menu_layer_get_layer(mainMenu));

  barcode_window = window_create();
  window_set_window_handlers(barcode_window, (WindowHandlers) {
    .load = barcode_window_load,
    .unload = barcode_window_unload,
  });

  app_message_register_inbox_received(app_received_msg);
  const uint32_t inbound_size = 124;
  const uint32_t outbound_size = 256;
  app_message_open(inbound_size, outbound_size);

  justSelected = -1;
  state = STATE_WAITING_BARCODE_LIST_LENGTH;
  send_request(REQUEST_BARCODE_LIST_LENGTH, WATCHAPP_VERSION_NUMBER);
}

static void window_unload(Window *window) {
	APP_LOG(APP_LOG_LEVEL_DEBUG, "calling window_unload");
    menu_layer_destroy(mainMenu);
	for (int i=0; i<barcodeNameCount; i++) {
	    free(nameBuffer[i]);
     	free(formatBuffer[i]);
	}
	window_destroy(barcode_window);
}

static void deinit(void) {
    APP_LOG(APP_LOG_LEVEL_DEBUG, "calling deinit");
    window_destroy(window);
}

int main(void) {

	state = STATE_NONE;

    window = window_create();
    window_set_window_handlers(window, (WindowHandlers) {
	    .load = window_load,
	    .unload = window_unload,
    });
	window_stack_push(window, true);

	app_event_loop();

	deinit();
}
