#version 150

uniform sampler2D img;
uniform float m_alpha;
in vec2 m_cord;
out vec4 fragColor;

void main()
{
  fragColor = texture(img, m_cord);
  fragColor.a = m_alpha;
#if defined(KODI_CLAMP_OUTPUT)
  fragColor = clamp(fragColor, 0.0, 1.0);
#endif
}
