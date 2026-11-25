#include "render/gl_handle.h"

#include <glad/gl.h>

namespace dx::gl {
    void deleteTexture(uint32_t id)
    {
        glDeleteTextures(1, &id);
    }

    void deleteFramebuffer(uint32_t id)
    {
        glDeleteFramebuffers(1, &id);
    }

    void deleteRenderbuffer(uint32_t id)
    {
        glDeleteRenderbuffers(1, &id);
    }

    void deleteVertexArray(uint32_t id)
    {
        glDeleteVertexArrays(1, &id);
    }

    void deleteBuffer(uint32_t id)
    {
        glDeleteBuffers(1, &id);
    }

    void deleteSampler(uint32_t id)
    {
        glDeleteSamplers(1, &id);
    }

    void deleteProgram(uint32_t id)
    {
        glDeleteProgram(id);
    }

    void deleteShader(uint32_t id)
    {
        glDeleteShader(id);
    }

    void deleteQuery(uint32_t id)
    {
        glDeleteQueries(1, &id);
    }

    TextureHandle createTexture()
    {
        GLuint id = 0;
        glGenTextures(1, &id);
        return TextureHandle(id);
    }

    FramebufferHandle createFramebuffer()
    {
        GLuint id = 0;
        glGenFramebuffers(1, &id);
        return FramebufferHandle(id);
    }

    RenderbufferHandle createRenderbuffer()
    {
        GLuint id = 0;
        glGenRenderbuffers(1, &id);
        return RenderbufferHandle(id);
    }

    VertexArrayHandle createVertexArray()
    {
        GLuint id = 0;
        glGenVertexArrays(1, &id);
        return VertexArrayHandle(id);
    }

    BufferHandle createBuffer()
    {
        GLuint id = 0;
        glGenBuffers(1, &id);
        return BufferHandle(id);
    }

    SamplerHandle createSampler()
    {
        GLuint id = 0;
        glGenSamplers(1, &id);
        return SamplerHandle(id);
    }

    QueryHandle createQuery()
    {
        GLuint id = 0;
        glGenQueries(1, &id);
        return QueryHandle(id);
    }
}
