#pragma once

#include <cstdint>

namespace dx {
    // The deleters behind the handles below. They live in gl_handle.cc so that holding a handle
    // does not pull the GL loader into every header that does.
    namespace gl {
        void deleteTexture(uint32_t id);
        void deleteFramebuffer(uint32_t id);
        void deleteRenderbuffer(uint32_t id);
        void deleteVertexArray(uint32_t id);
        void deleteBuffer(uint32_t id);
        void deleteSampler(uint32_t id);
        void deleteProgram(uint32_t id);
        void deleteShader(uint32_t id);
        void deleteQuery(uint32_t id);
    }

    // Owns one GL object name and deletes it when the handle is reset, reassigned or destroyed.
    // Zero is the empty handle, as it is to GL. Handles move but do not copy, so an object has
    // one owner; the raw name is lent out with get() for binding and attaching.
    template <void (*Delete)(uint32_t)>
    class GLHandle {
    public:
        GLHandle() = default;

        explicit GLHandle(uint32_t id)
            : value(id)
        {
        }

        ~GLHandle()
        {
            reset();
        }

        GLHandle(const GLHandle&) = delete;
        GLHandle& operator=(const GLHandle&) = delete;

        GLHandle(GLHandle&& other) noexcept
            : value(other.release())
        {
        }

        GLHandle& operator=(GLHandle&& other) noexcept
        {
            if (this != &other) {
                reset(other.release());
            }
            return *this;
        }

        uint32_t get() const
        {
            return value;
        }

        explicit operator bool() const
        {
            return value != 0;
        }

        // Gives up the name without deleting it.
        uint32_t release()
        {
            uint32_t id = value;
            value = 0;
            return id;
        }

        // Deletes the name held, if any, and takes the one given.
        void reset(uint32_t id = 0)
        {
            if (value && value != id) {
                Delete(value);
            }
            value = id;
        }

    private:
        uint32_t value = 0;
    };

    using TextureHandle = GLHandle<gl::deleteTexture>;
    using FramebufferHandle = GLHandle<gl::deleteFramebuffer>;
    using RenderbufferHandle = GLHandle<gl::deleteRenderbuffer>;
    using VertexArrayHandle = GLHandle<gl::deleteVertexArray>;
    using BufferHandle = GLHandle<gl::deleteBuffer>;
    using SamplerHandle = GLHandle<gl::deleteSampler>;
    using ProgramHandle = GLHandle<gl::deleteProgram>;
    using ShaderHandle = GLHandle<gl::deleteShader>;
    using QueryHandle = GLHandle<gl::deleteQuery>;

    // New names for each kind, from glGen*.
    namespace gl {
        TextureHandle createTexture();
        FramebufferHandle createFramebuffer();
        RenderbufferHandle createRenderbuffer();
        VertexArrayHandle createVertexArray();
        BufferHandle createBuffer();
        SamplerHandle createSampler();
        QueryHandle createQuery();
    }

    // A vertex array and the buffer behind it, made once for level geometry that does not change.
    struct StaticGeometry {
        VertexArrayHandle vao;
        BufferHandle buffer;

        uint32_t id() const
        {
            return vao.get();
        }

        explicit operator bool() const
        {
            return static_cast<bool>(vao);
        }
    };
}
