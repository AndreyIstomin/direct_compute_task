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
	float4 sunDir;
};

[numthreads(THREAD_X, THREAD_Y, 1)]
void csComputeSelfShadowing(uint groupIndex : SV_GroupIndex)
{
	sbShadows[groupIndex] = sunDir.x;
}

