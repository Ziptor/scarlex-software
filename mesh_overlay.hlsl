struct Vertex {
    float4 position : POSITION;
    float4 color : COLOR0;
};

struct Pixel {
    float4 position : SV_Position;
    float4 color : COLOR0;
};

Pixel VSMain(Vertex vertex) {
    Pixel output;
    output.position = vertex.position;
    output.color = vertex.color;
    return output;
}

float4 PSMain(Pixel pixel) : SV_Target {
    return float4(pixel.color.rgb * pixel.color.a, pixel.color.a);
}
