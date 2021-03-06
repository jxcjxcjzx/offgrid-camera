#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <memory.h>
#include <errno.h>
#include <sysexits.h>
#include <signal.h>
#include <math.h>
#include <iostream>
#include <string>
#include <algorithm>

#include <node.h>
#include <v8.h>

#include "bcm_host.h"
#include "interface/vcos/vcos.h"

#include "interface/mmal/mmal.h"
#include "interface/mmal/mmal_logging.h"
#include "interface/mmal/mmal_buffer.h"
#include "interface/mmal/util/mmal_util.h"
#include "interface/mmal/util/mmal_util_params.h"
#include "interface/mmal/util/mmal_default_components.h"
#include "interface/mmal/util/mmal_connection.h"

extern "C" {
#include "raspicam/RaspiCamControl.h"
#include "raspicam/RaspiPreview.h"
#include "raspicam/RaspiCLI.h"
#include "raspicam/RaspiTex.h"
#include "raspicam/RaspiTexUtil.h"
#include "raspicam/tga.h"
}

#include <semaphore.h>

#define VERSION_STRING "v1.3.8"

/// Camera number to use - we only have one camera, indexed from 0.
#define CAMERA_NUMBER 0

// Standard port setting for the camera component
#define MMAL_CAMERA_PREVIEW_PORT 0
#define MMAL_CAMERA_VIDEO_PORT 1
#define MMAL_CAMERA_CAPTURE_PORT 2


// Stills format information
// 0 implies variable
#define STILLS_FRAME_RATE_NUM 0
#define STILLS_FRAME_RATE_DEN 1

/// Video render needs at least 2 buffers.
#define VIDEO_OUTPUT_BUFFERS_NUM 3

using namespace v8;

class OffGrid;

int mmal_status_to_int(MMAL_STATUS_T status);
static void signal_handler(int signal_number);
static MMAL_STATUS_T create_camera_component(OffGrid *state);
static int parse_cmdline(int argc, const char **argv, OffGrid *state);
static void display_valid_parameters(const char *app_name);

/** Structure containing all state information for the current run
 */
class OffGrid {
public:
    uint32_t width;
    uint32_t height;
    int verbose;                        /// !0 if want detailed run information

    RASPIPREVIEW_PARAMETERS preview_parameters;    /// Preview setup parameters
    RASPICAM_CAMERA_PARAMETERS camera_parameters; /// Camera setup parameters

    MMAL_COMPONENT_T *camera_component;    /// Pointer to the camera component
    MMAL_COMPONENT_T *null_sink_component; /// Pointer to the null sink component
    MMAL_CONNECTION_T *preview_connection; /// Pointer to the connection from camera to preview

    RASPITEX_STATE raspitex_state; /// GL renderer state and parameters

    OffGrid() : tareBuffer(NULL)
              , tareSize(0)
              , xyData(NULL)
              , xyCount(0)
    {
        bcm_host_init();

        // Register our application with the logging system
        vcos_log_register("OffGrid", VCOS_LOG_CATEGORY);

        signal(SIGINT, signal_handler);

        // Disable USR1 for the moment - may be reenabled if go in to signal capture mode
        signal(SIGUSR1, SIG_IGN);

        set_defaults();
    }

    void init(int argc, const char *argv[]) {
        // Do we have any parameters
        if (argc == 1) {
            fprintf(stderr, "\%s Camera App %s\n\n", basename(argv[0]), VERSION_STRING);
            display_valid_parameters(basename(argv[0]));
            exit(EX_USAGE);
        }

        // Parse the command line and put options in to our status structure
        if (parse_cmdline(argc, argv, this)) {
            exit(EX_USAGE);
        }

        if (verbose) {
            fprintf(stderr, "\n%s Camera App %s\n\n", basename(argv[0]), VERSION_STRING);
        }

        raspitex_init(&raspitex_state);

        // OK, we have a nice set of parameters. Now set up our components
        // We have three components. Camera, Preview and encoder.
        // Camera and encoder are different in stills/video, but preview
        // is the same so handed off to a separate module

        if (create_camera_component(this) != MMAL_SUCCESS) {
            vcos_log_error("%s: Failed to create camera component", __func__);
        }

        if (raspicamcontrol_set_awb_mode(camera_component,
                                         MMAL_PARAM_AWBMODE_TUNGSTEN)
            != MMAL_SUCCESS) {
            fprintf(stderr, "failed to set white balance\n");
            exit(-1);
        }

        if (raspicamcontrol_set_exposure_mode(camera_component,
                                              MMAL_PARAM_EXPOSUREMODE_AUTO)
            != MMAL_SUCCESS) {
            fprintf(stderr, "failed to set exposure\n");
            exit(-1);
        }

        if (raspicamcontrol_set_ISO(camera_component, 400)
            != MMAL_SUCCESS) {
            fprintf(stderr, "failed to set ISO\n");
            exit(-1);
        }

        /* If GL preview is requested then start the GL threads */
        if (raspitex_start(&raspitex_state) != 0) {
            fprintf(stderr, "failed to start raspitex\n");
            exit(-1);
        }

        width = raspitex_state.width;
        height = raspitex_state.height;
        setWindow(0, 0, width, height);
    }

    void set_defaults() {
        verbose = 0;
        camera_component = NULL;
        preview_connection = NULL;

        // Setup preview window defaults
        raspipreview_set_defaults(&preview_parameters);

        // Set up the camera_parameters to default
        raspicamcontrol_set_defaults(&camera_parameters);

        // Set initial GL preview state
        raspitex_set_defaults(&raspitex_state);
    }

    void tare() {
        free(tareBuffer);
        tareBuffer = raspitex_capture_to_buffer(&raspitex_state, &tareSize);
    }

    void setWindow(uint32_t x1, uint32_t y1,
                   uint32_t x2, uint32_t y2) {
        uint32_t zero = 0;
        uint32_t w = raspitex_state.width;
        uint32_t h = raspitex_state.height;

        winX1 = std::min(std::max(zero, x1), w);
        winY1 = std::min(std::max(zero, y1), h);
        winX2 = std::min(std::max(winX1, x2), w);
        winY2 = std::min(std::max(winY1, y2), h);
    }

    bool setData(Isolate *isolate, const Handle<Array>& input) {
        delete[] xyData;
        xyCount = input->Length();
        xyData = new Datum[xyCount];
        Local<Array> output = Array::New(isolate, xyCount);
        rgbOutput.Reset(isolate, output);

        uint32_t xMax = 0;
        uint32_t yMax = 0;

        for (size_t i = 0; i < xyCount; ++i) {
            Handle<Array> pair = Handle<Array>::Cast(input->Get(i));

            uint32_t x = pair->Get(0)->Uint32Value();
            if (x > xMax) {
                xMax = x;
            }

            uint32_t y = pair->Get(1)->Uint32Value();
            if (y > yMax) {
                yMax = y;
            }

            xyData[i].x = x;
            xyData[i].y = y;
        }

        uint32_t xAdjust = (raspitex_state.width - xMax) >> 1;
        uint32_t yAdjust = (raspitex_state.height - yMax) >> 1;

        for (size_t i = 0; i < xyCount; ++i) {
            xyData[i].x += xAdjust;
            xyData[i].y += yAdjust;

            output->Set(i, Array::New(isolate, 3));
        }

        return true;
    }

    Handle<Array> sample(Isolate *isolate) {
        if (xyData == NULL) {
            return Array::New(isolate, 0);
        }

        Handle<Array> output = Handle<Array>::New(isolate, rgbOutput);

        size_t size;
        uint8_t *buffer = raspitex_capture_to_buffer(&raspitex_state, &size);

        for (size_t i = 0; i < xyCount; ++i) {
            uint32_t x = xyData[i].x;
            uint32_t y = xyData[i].y;

            uint8_t denominator = 0;

            uint32_t rSum = 0;
            uint32_t gSum = 0;
            uint32_t bSum = 0;

            for (uint32_t a = x - 1; a <= x + 1; ++a) {
                for (uint32_t b = y - 1; b <= y + 1; ++b) {
                    size_t offset = (y * raspitex_state.width + x) << 2;

                    uint8_t coefficient = 1;
                    if (a == x && b == y) {
                        coefficient = 4;
                    }

                    rSum += coefficient * buffer[offset + 0];
                    gSum += coefficient * buffer[offset + 1];
                    bSum += coefficient * buffer[offset + 2];

                    denominator += coefficient;
                }
            }

            Local<Array> rgb = Local<Array>::Cast(output->Get(i));
            rgb->Set(0, Number::New(isolate, rSum / denominator));
            rgb->Set(1, Number::New(isolate, gSum / denominator));
            rgb->Set(2, Number::New(isolate, bSum / denominator));
        }

        free(buffer);

        return output;
    }

    bool find(double rWeight, double gWeight, double bWeight,
              double &xResult, double &yResult) {
        if (!tareBuffer) {
            return false;
        }

        size_t size = 0;
        uint8_t *currBuffer = raspitex_capture_to_buffer(&raspitex_state, &size);
        uint32_t count = 0;
        double xSum = 0;
        double ySum = 0;
        double denominator = 0;

        double threshold = 0.5 *
            (255 * rWeight +
             255 * gWeight +
             255 * bWeight);

        // fprintf(stderr, "threshold: %g\n", threshold);
        // fprintf(stderr, "x1,y1,x2,y2,w,h: %d,%d,%d,%d,%d,%d\n",
        //         winX1, winY1, winX2, winY2,
        //         raspitex_state.width,
        //         raspitex_state.height);

        for (uint32_t x = winX1; x < winX2; ++x) {
            for (uint32_t y = winY1; y < winY2; ++y) {
                size_t offset = (y * raspitex_state.width + x) << 2;

                double rDelta = rWeight *
                    (currBuffer[offset + 0] -
                     tareBuffer[offset + 0]);

                double gDelta = gWeight *
                    (currBuffer[offset + 1] -
                     tareBuffer[offset + 1]);

                double bDelta = bWeight *
                    (currBuffer[offset + 2] -
                     tareBuffer[offset + 2]);

                double sum = rDelta + gDelta + bDelta;

                if (sum > threshold) {
                    xSum += sum * x;
                    ySum += sum * y;
                    denominator += sum;
                    // fprintf(stderr, "%d %d %g\n", x, y, sum);
                    ++count;
                }
            }
        }

        free(tareBuffer);
        tareBuffer = currBuffer;

        fprintf(stderr, "count: %d\n", count);

        if (count > 5) {
            xResult = xSum / denominator;
            yResult = ySum / denominator;
            // setWindow(std::floor(xResult) - 100,
            //           std::floor(yResult) - 100,
            //           std::floor(xResult) + 100,
            //           std::floor(yResult) + 100);
            return true;
        }

        // setWindow(0, 0, raspitex_state.width, raspitex_state.height);

        return false;
    }

    void switch_scene() {
        if (raspitex_state.scene_id != RASPITEX_SCENE_SHOWTIME) {
            raspitex_state.scene_id = RASPITEX_SCENE_SHOWTIME;
        } else {
            raspitex_state.scene_id = RASPITEX_SCENE_CALIBRATION;
        }

        raspitex_restart(&raspitex_state);
    }

    void save(const std::string& filename) {
        if (!tareBuffer) {
            return;
        }

        raspitexutil_brga_to_rgba(tareBuffer, tareSize);
        FILE* fd = fopen(filename.c_str(), "w+");
        write_tga(fd, width, height, tareBuffer, tareSize);
        fflush(fd);
        fclose(fd);
    }

    ~OffGrid() {
        if (verbose)
            fprintf(stderr, "Closing down\n");

        raspitex_stop(&raspitex_state);
        raspitex_destroy(&raspitex_state);

        // Disable ports that are not handled by connections.
        MMAL_PORT_T *port = camera_component->output[MMAL_CAMERA_VIDEO_PORT];
        if (port && port->is_enabled)
            mmal_port_disable(port);

        if (preview_connection)
            mmal_connection_destroy(preview_connection);

        if (preview_parameters.preview_component)
            mmal_component_disable(preview_parameters.preview_component);

        if (camera_component)
            mmal_component_disable(camera_component);

        raspipreview_destroy(&preview_parameters);

        if (camera_component) {
            mmal_component_destroy(camera_component);
            camera_component = NULL;
        }

        if (verbose)
            fprintf(stderr, "Close down completed, all components disconnected, disabled and destroyed\n\n");
    }

private:
    uint32_t winX1, winX2, winY1, winY2;
    uint8_t *tareBuffer;
    size_t tareSize;

    typedef struct {
        uint32_t x, y;
    } Datum;
    Datum *xyData;
    size_t xyCount;
    Persistent<Array> rgbOutput;
};

/// Comamnd ID's and Structure defining our command line options
#define CommandHelp         0
#define CommandWidth        1
#define CommandHeight       2
#define CommandVerbose      6

static COMMAND_LIST cmdline_commands[] =
{
   { CommandHelp,    "-help",       "?",  "This help information", 0 },
   { CommandWidth,   "-width",      "w",  "Set image width <size>", 1 },
   { CommandHeight,  "-height",     "h",  "Set image height <size>", 1 },
   { CommandVerbose, "-verbose",    "v",  "Output verbose information during run", 0 },
   // When the program starts up, it should illuminate all the LEDs blue
   // so that we can adjust the camera to include as many of them as
   // possible in the frame. Then we press enter (TODO: Can we do this
   // without user input?) to tell it to perform calibration. When
   // calibration is done, it samples from the video preview.
};

static int cmdline_commands_size = sizeof(cmdline_commands) / sizeof(cmdline_commands[0]);

/**
 * Parse the incoming command line and put resulting parameters in to the state
 *
 * @param argc Number of arguments in command line
 * @param argv Array of pointers to strings from command line
 * @param state Pointer to state structure to assign any discovered parameters to
 * @return non-0 if failed for some reason, 0 otherwise
 */
static int parse_cmdline(int argc, const char **argv, OffGrid *state)
{
   // Parse the command line arguments.
   // We are looking for --<something> or -<abreviation of something>

   int valid = 1;
   int i;

   for (i = 1; i < argc && valid; i++)
   {
      int command_id, num_parameters;

      if (!argv[i])
         continue;

      if (argv[i][0] != '-')
      {
         valid = 0;
         continue;
      }

      // Assume parameter is valid until proven otherwise
      valid = 1;

      command_id = raspicli_get_command_id(cmdline_commands, cmdline_commands_size, &argv[i][1], &num_parameters);

      // If we found a command but are missing a parameter, continue (and we will drop out of the loop)
      if (command_id != -1 && num_parameters > 0 && (i + 1 >= argc) )
         continue;

      //  We are now dealing with a command line option
      switch (command_id)
      {
      case CommandHelp:
         display_valid_parameters(basename(argv[0]));
         // exit straight away if help requested
         return -1;

      case CommandWidth: // Width > 0
         if (sscanf(argv[i + 1], "%u", &state->width) != 1) {
           fprintf(stderr, "parsed width %d\n", state->width);
           valid = 0;
         } else {
           i++;
         }
         break;

      case CommandHeight: // Height > 0
        if (sscanf(argv[i + 1], "%u", &state->height) != 1) {
          fprintf(stderr, "parsed height %d\n", state->height);
          valid = 0;
        } else {
          i++;
        }
        break;

      case CommandVerbose: // display lots of data during run
         state->verbose = 1;
         break;

      default:
      {
         // Try parsing for any image specific parameters
         // result indicates how many parameters were used up, 0,1,2
         // but we adjust by -1 as we have used one already
         const char *second_arg = (i + 1 < argc) ? argv[i + 1] : NULL;
         int parms_used = raspicamcontrol_parse_cmdline(&state->camera_parameters, &argv[i][1], second_arg);

         // Still unused, try preview options
         if (!parms_used)
            parms_used = raspipreview_parse_cmdline(&state->preview_parameters, &argv[i][1], second_arg);

         // Still unused, try GL preview options
         if (!parms_used)
            parms_used = raspitex_parse_cmdline(&state->raspitex_state, &argv[i][1], second_arg);

         // If no parms were used, this must be a bad parameters
         if (!parms_used)
            valid = 0;
         else
            i += parms_used - 1;

         break;
      }
      }
   }

   /* GL preview parameters use preview parameters as defaults unless overriden */
   if (! state->raspitex_state.gl_win_defined)
   {
      state->raspitex_state.x       = state->preview_parameters.previewWindow.x;
      state->raspitex_state.y       = state->preview_parameters.previewWindow.y;
      state->raspitex_state.width   = state->preview_parameters.previewWindow.width;
      state->raspitex_state.height  = state->preview_parameters.previewWindow.height;
   }
   /* Also pass the preview information through so GL renderer can determine
    * the real resolution of the multi-media image */
   state->raspitex_state.preview_x       = state->preview_parameters.previewWindow.x;
   state->raspitex_state.preview_y       = state->preview_parameters.previewWindow.y;
   state->raspitex_state.preview_width   = state->preview_parameters.previewWindow.width;
   state->raspitex_state.preview_height  = state->preview_parameters.previewWindow.height;
   state->raspitex_state.opacity         = state->preview_parameters.opacity;
   state->raspitex_state.verbose         = state->verbose;

   if (!valid)
   {
      fprintf(stderr, "Invalid command line option (%s)\n", argv[i-1]);
      return 1;
   }

   return 0;
}

/**
 * Display usage information for the application to stdout
 *
 * @param app_name String to display as the application name
 */
static void display_valid_parameters(const char *app_name)
{
   fprintf(stderr, "Runs camera for specific time, and take JPG capture at end if requested\n\n");
   fprintf(stderr, "usage: %s [options]\n\n", app_name);

   fprintf(stderr, "Image parameter commands\n\n");

   raspicli_display_help(cmdline_commands, cmdline_commands_size);

   // Help for preview options
   raspipreview_display_help();

   // Now display any help information from the camcontrol code
   raspicamcontrol_display_help();

   // Now display GL preview help
   raspitex_display_help();

   fprintf(stderr, "\n");

   return;
}

/**
 *  buffer header callback function for camera control
 *
 *  No actions taken in current version
 *
 * @param port Pointer to port from which callback originated
 * @param buffer mmal buffer header pointer
 */
static void camera_control_callback(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
   if (buffer->cmd == MMAL_EVENT_PARAMETER_CHANGED)
   {
   }
   else
   {
      vcos_log_error("Received unexpected camera control callback event, 0x%08x", buffer->cmd);
   }

   mmal_buffer_header_release(buffer);
}

/**
 * Create the camera component, set up its ports
 *
 * @param state Pointer to state control struct. camera_component member set to the created camera_component if successfull.
 *
 * @return MMAL_SUCCESS if all OK, something else otherwise
 *
 */
static MMAL_STATUS_T create_camera_component(OffGrid *state)
{
   MMAL_COMPONENT_T *camera = 0;
   MMAL_ES_FORMAT_T *format;
   MMAL_PORT_T *preview_port = NULL, *video_port = NULL, *still_port = NULL;
   MMAL_STATUS_T status;

   /* Create the component */
   status = mmal_component_create(MMAL_COMPONENT_DEFAULT_CAMERA, &camera);

   if (status != MMAL_SUCCESS)
   {
      vcos_log_error("Failed to create camera component");
      goto error;
   }

   if (!camera->output_num)
   {
      status = MMAL_ENOSYS;
      vcos_log_error("Camera doesn't have output ports");
      goto error;
   }

   preview_port = camera->output[MMAL_CAMERA_PREVIEW_PORT];
   video_port = camera->output[MMAL_CAMERA_VIDEO_PORT];
   still_port = camera->output[MMAL_CAMERA_CAPTURE_PORT];

   // Enable the camera, and tell it its control callback function
   status = mmal_port_enable(camera->control, camera_control_callback);

   if (status != MMAL_SUCCESS)
   {
      vcos_log_error("Unable to enable control port : error %d", status);
      goto error;
   }

   { // set up the camera configuration
       MMAL_PARAMETER_CAMERA_CONFIG_T cam_config;

       cam_config.hdr.id = MMAL_PARAMETER_CAMERA_CONFIG;
       cam_config.hdr.size = sizeof(cam_config);

       cam_config.max_stills_w = state->width;
       cam_config.max_stills_h = state->height;
       cam_config.stills_yuv422 = 0;
       cam_config.one_shot_stills = 1;
       cam_config.max_preview_video_w =
           fmax(state->preview_parameters.previewWindow.width, state->width);
       cam_config.max_preview_video_h =
           fmax(state->preview_parameters.previewWindow.height, state->height);
       cam_config.num_preview_video_frames = 3;
       cam_config.stills_capture_circular_buffer_height = 0;
       cam_config.fast_preview_resume = 0;
       cam_config.use_stc_timestamp = MMAL_PARAM_TIMESTAMP_MODE_RESET_STC;

       mmal_port_parameter_set(camera->control, &cam_config.hdr);
   }

   raspicamcontrol_set_all_parameters(camera, &state->camera_parameters);

   // Now set up the port formats

   format = preview_port->format;
   format->encoding = MMAL_ENCODING_OPAQUE;
   format->encoding_variant = MMAL_ENCODING_I420;

   // Use a full FOV 4:3 mode
   format->es->video.width = VCOS_ALIGN_UP(state->preview_parameters.previewWindow.width, 32);
   format->es->video.height = VCOS_ALIGN_UP(state->preview_parameters.previewWindow.height, 16);
   format->es->video.crop.x = 0;
   format->es->video.crop.y = 0;
   format->es->video.crop.width = state->preview_parameters.previewWindow.width;
   format->es->video.crop.height = state->preview_parameters.previewWindow.height;
   format->es->video.frame_rate.num = PREVIEW_FRAME_RATE_NUM;
   format->es->video.frame_rate.den = PREVIEW_FRAME_RATE_DEN;

   status = mmal_port_format_commit(preview_port);
   if (status != MMAL_SUCCESS)
   {
      vcos_log_error("camera viewfinder format couldn't be set");
      goto error;
   }

   // Set the same format on the video  port (which we dont use here)
   mmal_format_full_copy(video_port->format, format);
   status = mmal_port_format_commit(video_port);

   if (status  != MMAL_SUCCESS)
   {
      vcos_log_error("camera video format couldn't be set");
      goto error;
   }

   // Ensure there are enough buffers to avoid dropping frames
   if (video_port->buffer_num < VIDEO_OUTPUT_BUFFERS_NUM)
      video_port->buffer_num = VIDEO_OUTPUT_BUFFERS_NUM;

   format = still_port->format;

   // Set our stills format on the stills (for encoder) port
   format->encoding = MMAL_ENCODING_OPAQUE;
   format->es->video.width = VCOS_ALIGN_UP(state->width, 32);
   format->es->video.height = VCOS_ALIGN_UP(state->height, 16);
   format->es->video.crop.x = 0;
   format->es->video.crop.y = 0;
   format->es->video.crop.width = state->width;
   format->es->video.crop.height = state->height;
   format->es->video.frame_rate.num = STILLS_FRAME_RATE_NUM;
   format->es->video.frame_rate.den = STILLS_FRAME_RATE_DEN;


   status = mmal_port_format_commit(still_port);

   if (status != MMAL_SUCCESS)
   {
      vcos_log_error("camera still format couldn't be set");
      goto error;
   }

   /* Ensure there are enough buffers to avoid dropping frames */
   if (still_port->buffer_num < VIDEO_OUTPUT_BUFFERS_NUM)
      still_port->buffer_num = VIDEO_OUTPUT_BUFFERS_NUM;

   /* Enable component */
   status = mmal_component_enable(camera);

   if (status != MMAL_SUCCESS)
   {
      vcos_log_error("camera component couldn't be enabled");
      goto error;
   }

   if (raspitex_configure_preview_port(&state->raspitex_state, preview_port) != 0) {
     fprintf(stderr, "Failed to configure preview port for GL rendering");
     goto error;
   }

   state->camera_component = camera;

   if (state->verbose)
      fprintf(stderr, "Camera component done\n");

   return status;

error:

   if (camera)
      mmal_component_destroy(camera);

   return status;
}

/**
 * Handler for sigint signals
 *
 * @param signal_number ID of incoming signal.
 *
 */
static void signal_handler(int signal_number)
{
   if (signal_number == SIGUSR1)
   {
      // Handle but ignore - prevents us dropping out if started in none-signal mode
      // and someone sends us the USR1 signal anyway
   }
   else
   {
      // Going to abort on all other signals
      vcos_log_error("Aborting program\n");
      exit(130);
   }
}

static OffGrid *sState = NULL;

static void Tare(const FunctionCallbackInfo<Value>& args) {
    sState->tare();
    args.GetReturnValue().Set(args.This());
}

static void SetData(const FunctionCallbackInfo<Value>& args) {
  Isolate *isolate = args.GetIsolate();
  Handle<Value> arg0 = args[0];

  if (! arg0->IsArray()) {
      args.GetReturnValue().Set(Boolean::New(isolate, false));
    return;
  }

  sState->setData(isolate, Handle<Array>::Cast(arg0));

  args.GetReturnValue().Set(Boolean::New(isolate, true));
}

static void Sample(const FunctionCallbackInfo<Value>& args) {
    args.GetReturnValue().Set(sState->sample(args.GetIsolate()));
}

static void Find(const FunctionCallbackInfo<Value>& args) {
    double x, y;

    if (sState->find(args[0]->NumberValue(),
                     args[1]->NumberValue(),
                     args[2]->NumberValue(),
                     x, y)) {
        Isolate *isolate = args.GetIsolate();
        Handle<Array> xy = Array::New(isolate, 2);
        xy->Set(0, Number::New(isolate, x));
        xy->Set(1, Number::New(isolate, y));
        args.GetReturnValue().Set(xy);
    }
}

static void Width(const FunctionCallbackInfo<Value>& args) {
    args.GetReturnValue().Set(Integer::New(args.GetIsolate(),
                                           sState->raspitex_state.width));
}

static void Height(const FunctionCallbackInfo<Value>& args) {
    args.GetReturnValue().Set(Integer::New(args.GetIsolate(),
                                           sState->raspitex_state.height));
}

static void Save(const FunctionCallbackInfo<Value>& args) {
    if (args.Length() > 0) {
        String::Utf8Value filename(args[0]->ToString());
        sState->save(std::string(*filename));
    }
    args.GetReturnValue().Set(args.This());
}

static void cleanup(void*) {
    delete sState;
}

void init(Handle<Object> target) {
    sState = new OffGrid();

    const char *argv[] = {
      "offgrid",
      "--glscene", "sobel",
      "--width", "1600",
      "--height", "1200",
      "--hflip", "--vflip",
      "--glwin", "0,0,1600,1200"
    };
    sState->init(11, argv);

    node::AtExit(cleanup);

    NODE_SET_METHOD(target, "tare", Tare);
    NODE_SET_METHOD(target, "setData", SetData);
    NODE_SET_METHOD(target, "sample", Sample);
    NODE_SET_METHOD(target, "find", Find);
    NODE_SET_METHOD(target, "save", Save);
    NODE_SET_METHOD(target, "width", Width);
    NODE_SET_METHOD(target, "height", Height);
}

NODE_MODULE(offgrid, init);
