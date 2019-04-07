//
// CUDA volume path tracing kernel interface
//

struct Kernel_params {
	// Display
	uint2 resolution;
	float exposure_scale;
	unsigned int *display_buffer;

	// Progressive rendering state
	unsigned int iteration;
	float3 *accum_buffer;

	// Limit on path length
	unsigned int max_interactions;	// Accumulation buffer count
	unsigned int ray_depth;			// Max number of bounces 

	//Volume parameters ( No absorbtion coefficient)

	float max_extinction;	// Extinction majorant
	float min_extinction;	// Extinction minorant
	float phase_g1;			// Phase function anisotropy (.0f means isotropic phase function)
	float phase_g2;			// Phase function anisotropy for double lobe phase functions
	float3 albedo;			// sigma_s / sigma_t
	float3 extinction;		// sigma_t
	
	// Environment
	unsigned int environment_type;
	float3 light_pos;
	float3 light_energy;
	cudaTextureObject_t env_tex;
		
};
 
extern "C" __global__ void volume_rt_kernel(const VDBInfo gvdb, const Kernel_params kernel_params);

