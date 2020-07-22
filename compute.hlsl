#define THREAD_X 32
#define THREAD_Y 32

struct Particle
{
	float3 pos;
	float radius;
	float opacity;
};

StructuredBuffer<Particle> sbParticles;

RWBuffer<float> sbShadows;

//float3 sunDir; <-- check it
cbuffer globals
{
	float3 sunDir;
	float reserved;
};

float Overlap(in Particle caster, in Particle receiver) {

	const float sunDistDiff = caster.pos.x - receiver.pos.x;
	const uint isCaster = sunDistDiff > 0;

	const float dist = length(receiver.pos.yz - caster.pos.yz);

	return isCaster * caster.opacity * min(caster.radius * caster.radius / (receiver.radius * receiver.radius), 1.0f) * 
		smoothstep(receiver.radius + caster.radius, abs(receiver.radius - caster.radius), dist);
}

groupshared Particle smem[THREAD_X * THREAD_Y];

[numthreads(THREAD_X, THREAD_Y, 1)]
void csComputeSelfShadowing(uint tid : SV_GroupIndex)
{
	float result = 1.0f;
	const uint N = THREAD_X * THREAD_Y;
	const float3 sunZ = normalize(cross(sunDir, float3(0.0f, 1.0f, 0.0f))); // Допускаю, что sunDir не колинеарен Y
	const float3 sunY = cross(sunZ, sunDir);


	Particle p = sbParticles[tid];
	p.pos = float3(dot(p.pos, sunDir), dot(p.pos, sunY), dot(p.pos, sunZ));
	
	smem[tid] = p;

	GroupMemoryBarrierWithGroupSync();

	for (uint i = tid + 1; i < N + tid; ++i) {
		result *= 1.0f - Overlap(smem[i % N], p);
	}

	sbShadows[tid] = result;
}
