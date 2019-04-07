//
// Small interactive application running the volume path tracer
//

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include <GL/glew.h>
#include <GLFW/glfw3.h>

#include <cuda.h>
#include <cuda_runtime.h>
#include <cuda_gl_interop.h>
//#include <vector_functions.h>

#define _USE_MATH_DEFINES
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <algorithm>

#include "gvdb.h"
#include "hdr_loader.h"
#include "render_kernel.h"

CUmodule		cuCustom;
CUfunction		cuRaycastKernel;
VolumeGVDB		gvdb;

#define check_success(expr) \
    do { \
        if(!(expr)) { \
            fprintf(stderr, "Error in file %s, line %u: \"%s\".\n", __FILE__, __LINE__, #expr); \
            exit(EXIT_FAILURE); \
        } \
    } while(false)


// Initialize gvdb volume 
static void init_gvdb()
{

	int cuda_devices[1];
	unsigned int num_cuda_devices;
	check_success(cudaGLGetDevices(&num_cuda_devices, cuda_devices, 1, cudaGLDeviceListAll) == cudaSuccess);
	if (num_cuda_devices == 0) {
		fprintf(stderr, "Could not determine CUDA device for current OpenGL context\n.");
		exit(EXIT_FAILURE);
	}
	gvdb.SetCudaDevice(cuda_devices[0]);
	gvdb.Initialize();

}

// Initialize GLFW and GLEW.
static GLFWwindow *init_opengl()
{
	check_success(glfwInit());
	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
	glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
	glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);

	GLFWwindow *window = glfwCreateWindow(
		1024, 1024, "volume path tracer", NULL, NULL);
	if (!window) {
		fprintf(stderr, "Error creating OpenGL window.\n");;
		glfwTerminate();
	}
	glfwMakeContextCurrent(window);

	const GLenum res = glewInit();
	if (res != GLEW_OK) {
		fprintf(stderr, "GLEW error: %s.\n", glewGetErrorString(res));
		glfwTerminate();
	}

	glfwSwapInterval(0);

	check_success(glGetError() == GL_NO_ERROR);
	return window;
}

// Initialize CUDA with OpenGL interop.
static void init_cuda()
{
	int cuda_devices[1];
	unsigned int num_cuda_devices;
	check_success(cudaGLGetDevices(&num_cuda_devices, cuda_devices, 1, cudaGLDeviceListAll) == cudaSuccess);
	if (num_cuda_devices == 0) {
		fprintf(stderr, "Could not determine CUDA device for current OpenGL context\n.");
		exit(EXIT_FAILURE);
	}
	check_success(cudaSetDevice(cuda_devices[0]) == cudaSuccess);
}

// Utility: add a GLSL shader.
static void add_shader(GLenum shader_type, const char *source_code, GLuint program)
{
	GLuint shader = glCreateShader(shader_type);
	check_success(shader);
	glShaderSource(shader, 1, &source_code, NULL);
	glCompileShader(shader);

	GLint success;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
	check_success(success);

	glAttachShader(program, shader);
	check_success(glGetError() == GL_NO_ERROR);
}

// Create a simple GL program with vertex and fragement shader for texture lookup.
static GLuint create_shader_program()
{
	GLint success;
	GLuint program = glCreateProgram();

	const char *vert =
		"#version 330\n"
		"in vec3 Position;\n"
		"out vec2 TexCoord;\n"
		"void main() {\n"
		"    gl_Position = vec4(Position, 1.0);\n"
		"    TexCoord = 0.5 * Position.xy + vec2(0.5);\n"
		"}\n";
	add_shader(GL_VERTEX_SHADER, vert, program);

	const char *frag =
		"#version 330\n"
		"in vec2 TexCoord;\n"
		"out vec4 FragColor;\n"
		"uniform sampler2D TexSampler;\n"
		"void main() {\n"
		"    FragColor = texture(TexSampler, TexCoord);\n"
		"}\n";
	add_shader(GL_FRAGMENT_SHADER, frag, program);

	glLinkProgram(program);
	glGetProgramiv(program, GL_LINK_STATUS, &success);
	if (!success) {
		fprintf(stderr, "Error linking shadering program\n");
		glfwTerminate();
	}

	glUseProgram(program);
	check_success(glGetError() == GL_NO_ERROR);

	return program;
}

// Create a quad filling the whole screen.
static GLuint create_quad(GLuint program, GLuint* vertex_buffer)
{
	static const float3 vertices[6] = {
		{ -1.f, -1.f, 0.0f },
		{ -1.f,  1.f, 0.0f },
		{  1.f, -1.f, 0.0f },
		{  1.f, -1.f, 0.0f },
		{  1.f,  1.f, 0.0f },
		{ -1.f,  1.f, 0.0f }
	};

	glGenBuffers(1, vertex_buffer);
	glBindBuffer(GL_ARRAY_BUFFER, *vertex_buffer);
	glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

	GLuint vertex_array;
	glGenVertexArrays(1, &vertex_array);
	glBindVertexArray(vertex_array);

	const GLint pos_index = glGetAttribLocation(program, "Position");
	glEnableVertexAttribArray(pos_index);
	glVertexAttribPointer(
		pos_index, 3, GL_FLOAT, GL_FALSE, sizeof(float3), 0);

	check_success(glGetError() == GL_NO_ERROR);

	return vertex_array;
}

// Context structure for window callback functions.
struct Window_context
{
	int zoom_delta;

	bool moving;
	bool panning;
	double move_start_x, move_start_y;
	double move_dx, move_dy, move_mx, move_my;

	float exposure;

	bool change;
	unsigned int config_type;
};

// GLFW scroll callback.
static void handle_scroll(GLFWwindow *window, double xoffset, double yoffset)
{
	Window_context *ctx = static_cast<Window_context *>(glfwGetWindowUserPointer(window));
	if (yoffset > 0.0)
		ctx->zoom_delta = 1;
	else if (yoffset < 0.0)
		ctx->zoom_delta = -1;
}

// GLFW keyboard callback.
static void handle_key(GLFWwindow *window, int key, int scancode, int action, int mods)
{
	if (action == GLFW_PRESS) {
		Window_context *ctx = static_cast<Window_context *>(glfwGetWindowUserPointer(window));
		Camera3D* cam = gvdb.getScene()->getCamera();
		float fov = cam->getFov();
		

		switch (key) {
		case GLFW_KEY_ESCAPE:
			glfwSetWindowShouldClose(window, GLFW_TRUE);
			break;
		case GLFW_KEY_KP_SUBTRACT:
		case GLFW_KEY_LEFT_BRACKET:
			fov -= 1.0f;
			cam->setFov(fov);
			ctx->change=true;
			break;
		case GLFW_KEY_KP_ADD:
		case GLFW_KEY_RIGHT_BRACKET:
			fov += 1.0f;
			cam->setFov(fov);
			ctx->change=true;
			break;
		case GLFW_KEY_SPACE:
			++ctx->config_type;
		default:
			break;
		}
	}
}

// GLFW mouse button callback.
static void handle_mouse_button(GLFWwindow *window, int button, int action, int mods)
{
	Window_context *ctx = static_cast<Window_context *>(glfwGetWindowUserPointer(window));
	bool imgui_hover = ImGui::IsMouseHoveringAnyWindow();
	if (button == GLFW_MOUSE_BUTTON_LEFT && !imgui_hover) {
		if (action == GLFW_PRESS) {
			ctx->moving = true;
			glfwGetCursorPos(window, &ctx->move_start_x, &ctx->move_start_y);
		}
		else
			ctx->moving = false;
	}

	if (button == GLFW_MOUSE_BUTTON_MIDDLE && !imgui_hover) {
		if (action == GLFW_PRESS) {
			ctx->panning = true;
			glfwGetCursorPos(window, &ctx->move_start_x, &ctx->move_start_y);
		}
		else
			ctx->panning = false;
	}
}

// GLFW mouse position callback.
static void handle_mouse_pos(GLFWwindow *window, double xpos, double ypos)
{
	Window_context *ctx = static_cast<Window_context *>(glfwGetWindowUserPointer(window));
	if (ctx->moving)
	{
		ctx->move_dx += xpos - ctx->move_start_x;
		ctx->move_dy += ypos - ctx->move_start_y;
		ctx->move_start_x = xpos;
		ctx->move_start_y = ypos;
	}
	if (ctx->panning)
	{
		ctx->move_mx += xpos - ctx->move_start_x;
		ctx->move_my += ypos - ctx->move_start_y;
		ctx->move_start_x = xpos;
		ctx->move_start_y = ypos;
	}
}

// Resize OpenGL and CUDA buffers for a given resolution.
static void resize_buffers(
	float3 **accum_buffer_cuda,
	cudaGraphicsResource_t *display_buffer_cuda, int width, int height, GLuint display_buffer)
{
	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, display_buffer);
	glBufferData(GL_PIXEL_UNPACK_BUFFER, width * height * 4, NULL, GL_DYNAMIC_COPY);
	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

	check_success(glGetError() == GL_NO_ERROR);

	if (*display_buffer_cuda)
		check_success(cudaGraphicsUnregisterResource(*display_buffer_cuda) == cudaSuccess);
	check_success(
		cudaGraphicsGLRegisterBuffer(
			display_buffer_cuda, display_buffer, cudaGraphicsRegisterFlagsWriteDiscard) == cudaSuccess);

	if (*accum_buffer_cuda)
		check_success(cudaFree(*accum_buffer_cuda) == cudaSuccess);
	check_success(cudaMalloc(accum_buffer_cuda, width * height * sizeof(float3)) == cudaSuccess);
}


// Create enviroment texture.
static bool create_environment(
	cudaTextureObject_t *env_tex,
	cudaArray_t *env_tex_data,
	const char *envmap_name)
{
	unsigned int rx, ry;
	float *pixels;
	if (!load_hdr_float4(&pixels, &rx, &ry, envmap_name)) {
		fprintf(stderr, "error loading environment map file %s\n", envmap_name);
		return false;
	}

	const cudaChannelFormatDesc channel_desc = cudaCreateChannelDesc<float4>();
	check_success(cudaMallocArray(env_tex_data, &channel_desc, rx, ry) == cudaSuccess);

	check_success(cudaMemcpyToArray(
		*env_tex_data, 0, 0, pixels,
		rx * ry * sizeof(float4), cudaMemcpyHostToDevice) == cudaSuccess);

	cudaResourceDesc res_desc;
	memset(&res_desc, 0, sizeof(res_desc));
	res_desc.resType = cudaResourceTypeArray;
	res_desc.res.array.array = *env_tex_data;

	cudaTextureDesc tex_desc;
	memset(&tex_desc, 0, sizeof(tex_desc));
	tex_desc.addressMode[0] = cudaAddressModeWrap;
	tex_desc.addressMode[1] = cudaAddressModeClamp;
	tex_desc.addressMode[2] = cudaAddressModeWrap;
	tex_desc.filterMode = cudaFilterModeLinear;
	tex_desc.readMode = cudaReadModeElementType;
	tex_desc.normalizedCoords = 1;

	check_success(cudaCreateTextureObject(env_tex, &res_desc, &tex_desc, NULL) == cudaSuccess);
	return true;
}


// Process camera movement.
static void update_camera(
	double dx,
	double dy,
	double mx,
	double my,
	int zoom_delta)
{
	Camera3D* cam = gvdb.getScene()->getCamera();
	Vector3DF angs = cam->getAng();
	float dist = cam->getOrbitDist();
	dist -= zoom_delta*300;
	angs.x -= dx * 0.05f;
	angs.y -= dy * 0.05f;
	cam->setOrbit(angs, cam->getToPos(), dist, cam->getDolly());
	cam->moveRelative(float(mx) * dist / 200, float(my) * dist / 200, 0);
}

int main(const int argc, const char* argv[])
{
	Window_context window_context;
	memset(&window_context, 0, sizeof(Window_context));

	GLuint display_buffer = 0;
	GLuint display_tex = 0;
	GLuint program = 0;
	GLuint quad_vertex_buffer = 0;
	GLuint quad_vao = 0;
	GLFWwindow *window = NULL;
	int width = -1;
	int height = -1;
	window_context.change = false;

	// Init OpenGL window and callbacks.
	window = init_opengl();
	glfwSetWindowUserPointer(window, &window_context);
	glfwSetKeyCallback(window, handle_key);
	glfwSetScrollCallback(window, handle_scroll);
	glfwSetCursorPosCallback(window, handle_mouse_pos);
	glfwSetMouseButtonCallback(window, handle_mouse_button);

	glGenBuffers(1, &display_buffer);
	glGenTextures(1, &display_tex);
	check_success(glGetError() == GL_NO_ERROR);

	program = create_shader_program();
	quad_vao = create_quad(program, &quad_vertex_buffer);

	init_cuda();

	float3 *accum_buffer = NULL;
	cudaGraphicsResource_t display_buffer_cuda = NULL;

	// SETUP IMGUI PARAMETERS
	const char* glsl_version = "#version 330";

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO &io = ImGui::GetIO(); (void)io;
	io.ConfigWindowsMoveFromTitleBarOnly = true;
	ImGui::StyleColorsDark();

	ImGui_ImplGlfw_InitForOpenGL(window, true);
	ImGui_ImplOpenGL3_Init(glsl_version);

	// SETUP GVDB PARAMETERS


	printf("Initializing GVDB volume object ");
	init_gvdb();
	gvdb.AddPath(ASSET_PATH);

	char scnpath[1024];
	if (!gvdb.FindFile("wdas_cloud_sixteenth.vdb", scnpath)) {
		printf("Cannot find vdb file.\n");
		exit(-1);
	}
	printf("Loading VDB. %s\n", scnpath);
	gvdb.LoadVDB(scnpath);
	gvdb.SetTransform(Vector3DF(0,0,0), Vector3DF(0.1,0.1,0.1), Vector3DF(0, 0, 0), Vector3DF(0, 0, 0));
	
	gvdb.Measure(true);

	Camera3D* cam = new Camera3D;
	cam->setFov(35);
	cam->setOrbit(Vector3DF(98.0f, 0, 0), Vector3DF(2000, 100,0), 400, 1.0);
	gvdb.getScene()->SetCamera(cam);
	
	printf("Loading module: render_kernel.ptx\n");
	cuModuleLoad(&cuCustom, "render_kernel.ptx");
	cuModuleGetFunction(&cuRaycastKernel, cuCustom, "volume_rt_kernel");
	
	gvdb.SetModule(cuCustom);

	gvdb.mbProfile = true;
	gvdb.PrepareRender(1024, 1024, gvdb.getScene()->getShading());
	gvdb.PrepareVDB();
	char *vdbinfo = gvdb.getVDBInfo();

	// END GVDB PARAMETERS


	// Setup initial CUDA kernel parameters.
	Kernel_params kernel_params;
	memset(&kernel_params, 0, sizeof(Kernel_params));
	kernel_params.iteration = 0;
	kernel_params.max_interactions = 1;
	kernel_params.exposure_scale = 1.0f;
	kernel_params.environment_type = 0;
	kernel_params.max_extinction = 0.3f;
	kernel_params.phase_g1 = 0.0f;
	kernel_params.phase_g2 = 0.0f;
	kernel_params.albedo = make_float3(1.0f, 1.0f, 1.0f);
	kernel_params.extinction = make_float3(1.0f, 1.0f, 1.0f);
	kernel_params.light_pos = make_float3(0, 1000, 0);
	kernel_params.light_energy = make_float3(1.0f, 1.0f, 1.0f);
	cudaArray_t env_tex_data = 0;
	bool env_tex = false;
	
	// Imgui Parameters
	
	int max_interaction = 1; 
	float max_extinction = 0.1;
	ImVec4 light_pos = ImVec4(0.0f, 1000.0f, 0.0f, 1.00f);
	ImVec4 light_energy = ImVec4(0.0f, 0.0f, 0.0f, 1.00f);
	 
	// End ImGui parameters
	
	if (argc >= 2)
		env_tex = create_environment(
			&kernel_params.env_tex, &env_tex_data, argv[1]);
	if (env_tex) {
		kernel_params.environment_type = 1;
		window_context.config_type = 2;
	}
	while (!glfwWindowShouldClose(window)) {

		// Process events.
		glfwPollEvents();
		Window_context *ctx = static_cast<Window_context *>(glfwGetWindowUserPointer(window));
		
		// Update kernel params
		kernel_params.exposure_scale = powf(2.0f, ctx->exposure);
		kernel_params.max_interactions = max_interaction;

		const unsigned int volume_type = ctx->config_type & 1;
		const unsigned int environment_type = env_tex ? ((ctx->config_type >> 1) & 1) : 0;
		
		// Draw imgui 
		
		ImGui_ImplOpenGL3_NewFrame();
		ImGui_ImplGlfw_NewFrame();
		ImGui::NewFrame();

		ImGui::Begin("Debug window"); 
		ImGui::SliderFloat("exposure", &ctx->exposure, -10.0f, 10.0f);
		ImGui::InputInt("max_interactions", &max_interaction, 1);
		ImGui::InputFloat("extinction maj.", &kernel_params.max_extinction, .0f, 1.0f);
		ImGui::SliderFloat("phase anistropy", &kernel_params.phase_g1, -1.0f, 1.0f);
		ImGui::ColorEdit3("Volume Extinction", (float *)&kernel_params.extinction);
		ImGui::ColorEdit3("Volume Color", (float *)&kernel_params.albedo);

		ImGui::ColorEdit3("Light Color", (float *)&kernel_params.light_energy);
		ImGui::InputFloat3("light position", (float *)&kernel_params.light_pos, 3);
		ImGui::Text("Application average %.3f ms/frame (%.1f FPS)", 1000.0f / ImGui::GetIO().Framerate, ImGui::GetIO().Framerate);
		ImGui::End();
		ImGui::Render();


		if (ctx->change || max_interaction != kernel_params.max_interactions) {

			gvdb.PrepareRender(width, height, gvdb.getScene()->getShading());
			kernel_params.iteration = 0;
			ctx->change = false;

		}	
		// Reallocate buffers if window size changed.
		int nwidth, nheight;
		glfwGetFramebufferSize(window, &nwidth, &nheight);
		if (nwidth != width || nheight != height)
		{
			width = nwidth;
			height = nheight;

			gvdb.PrepareRender(width, height, gvdb.getScene()->getShading());
			resize_buffers(&accum_buffer, &display_buffer_cuda, width, height, display_buffer);
			kernel_params.accum_buffer = accum_buffer;

			glViewport(0, 0, width, height);

			kernel_params.resolution.x = width;
			kernel_params.resolution.y = height;
			kernel_params.iteration = 0;
		}

		if (ctx->move_dx != 0.0 || ctx->move_dy != 0.0 || ctx->move_mx != 0.0 || ctx->move_my != 0.0 || ctx->zoom_delta) {

			
			update_camera(ctx->move_dx, ctx->move_dy , ctx->move_mx, ctx->move_my, ctx->zoom_delta);
			ctx->move_dx = ctx->move_dy = ctx->move_mx = ctx->move_my = 0.0;
			ctx->zoom_delta = 0;
			gvdb.PrepareRender(width, height, gvdb.getScene()->getShading());

			kernel_params.iteration = 0;
		}

		// Map GL buffer for access with CUDA.
		check_success(cudaGraphicsMapResources(1, &display_buffer_cuda, /*stream=*/0) == cudaSuccess);
		void *p;
		size_t size_p;
		
		cudaGraphicsResourceGetMappedPointer(&p, &size_p, display_buffer_cuda);
		//printf("error in: %s\n", cudaGetErrorString(cudaGetLastError()));
		kernel_params.display_buffer = reinterpret_cast<unsigned int *>(p);

		// Launch volume rendering kernel.
		Vector3DI block(8, 8, 1);
		Vector3DI grid(int(width / block.x) + 1, int(height / block.y) + 1, 1);
		dim3 threads_per_block(16, 16);
		dim3 num_blocks((width + 15) / 16, (height + 15) / 16);
		void *params[] = {vdbinfo, &kernel_params };
		//check_success(cudaLaunchKernel((const void *)&volume_rt_kernel,	num_blocks,	threads_per_block,	params) == cudaSuccess);
		cuLaunchKernel(cuRaycastKernel, grid.x, grid.y, 1, block.x, block.y, 1, 0, NULL, params, NULL);
		++kernel_params.iteration;

		// Unmap GL buffer.
		check_success(cudaGraphicsUnmapResources(1, &display_buffer_cuda, /*stream=*/0) == cudaSuccess);

		// Update texture for display.
		glBindBuffer(GL_PIXEL_UNPACK_BUFFER, display_buffer);
		glBindTexture(GL_TEXTURE_2D, display_tex);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
		check_success(glGetError() == GL_NO_ERROR);

		// Render the quad.
		
		glClear(GL_COLOR_BUFFER_BIT);
		glBindVertexArray(quad_vao);
		glDrawArrays(GL_TRIANGLES, 0, 6);
		check_success(glGetError() == GL_NO_ERROR);

		ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

		glfwSwapBuffers(window);
	}

	//Cleanup imgui
	ImGui_ImplOpenGL3_Shutdown();
	ImGui_ImplGlfw_Shutdown();
	ImGui::DestroyContext();

	// Cleanup CUDA.
	if (env_tex) {
		check_success(cudaDestroyTextureObject(kernel_params.env_tex) == cudaSuccess);
		check_success(cudaFreeArray(env_tex_data) == cudaSuccess);
	}
	check_success(cudaFree(accum_buffer) == cudaSuccess);

	// Cleanup OpenGL.
	glDeleteVertexArrays(1, &quad_vao);
	glDeleteBuffers(1, &quad_vertex_buffer);
	glDeleteProgram(program);
	check_success(glGetError() == GL_NO_ERROR);
	glfwDestroyWindow(window);
	glfwTerminate();

	return 0;
}
