using Cortex.Vrm;
if(args.Length!=1)throw new ArgumentException("Expected C++ motion fixture");
byte[] bytes=File.ReadAllBytes(args[0]);
if(!MotionPacket.TryParse(bytes,out var packet)||packet.Sequence!=17||packet.TimestampUs!=123456789)
    throw new Exception("C++/C# wire mismatch");
if(Math.Abs(packet.Values[3*5+2])<.6f)throw new Exception("Elbow quaternion lost across languages");
if(MotionPacket.TryParse(bytes[..^1],out _))throw new Exception("Truncated packet accepted");
bytes[8]=2;if(MotionPacket.TryParse(bytes,out _))throw new Exception("Unknown version accepted");
bytes[8]=1;BitConverter.GetBytes(float.NaN).CopyTo(bytes,32);
if(MotionPacket.TryParse(bytes,out _))throw new Exception("NaN rotation accepted");
Console.WriteLine("PASS: C++ to C# golden fixture, quaternion values, size/version/NaN rejection");
foreach (int fps in new[]{30,60,120})
{
    var state = new BoneTrackingState();
    if(state.Observe(.45f,0))throw new Exception("Uncertain bone must not acquire tracking");
    if(!state.Observe(.9f,0)||!state.Observe(.4f,1.0/fps))throw new Exception("Tracking confidence hysteresis failed");
    double last=1.0/fps;
    for(int i=1;i<=3;++i)
    {
        double now=last+i/(double)fps;
        if(state.Observe(.1f,now)||state.Weight(now)!=1)throw new Exception("Brief occlusion snapped a tracked bone");
    }
    float previous=1;
    for(int i=1;i<=fps;++i)
    {
        float weight=state.Weight(last+i/(double)fps);
        if(weight>previous+1e-5f)throw new Exception("Cached pose refreshed tracking hold");
        previous=weight;
    }
    if(previous!=0||state.Observe(.4f,2))throw new Exception("Lost bone must fade and require confident reacquisition");
    if(!state.Observe(.9f,2)||state.Observe(float.NaN,2.01))throw new Exception("Reacquisition/invalid confidence failed");
}
Console.WriteLine("PASS: 30/60/120fps confidence hysteresis, 3-frame occlusion hold, loss decay and reacquisition");
