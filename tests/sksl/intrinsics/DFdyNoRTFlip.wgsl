### Compilation failed:

error: :18:20 error: unresolved call target 'dFdy'
    let _skTemp0 = dFdy(_globalUniforms.testInputs.x);
                   ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^


struct FSIn {
  @builtin(front_facing) sk_Clockwise: bool,
  @builtin(position) sk_FragCoord: vec4<f32>,
};
struct FSOut {
  @location(0) sk_FragColor: vec4<f32>,
};
struct _GlobalUniforms {
  testInputs: vec4<f32>,
  colorGreen: vec4<f32>,
  colorRed: vec4<f32>,
};
@binding(0) @group(0) var<uniform> _globalUniforms: _GlobalUniforms;
fn main(_skParam0: vec2<f32>) -> vec4<f32> {
  let coords = _skParam0;
  {
    var expected: vec4<f32> = vec4<f32>(0.0);
    let _skTemp0 = dFdy(_globalUniforms.testInputs.x);
    let _skTemp1 = dFdy(_globalUniforms.testInputs.xy);
    let _skTemp2 = dFdy(_globalUniforms.testInputs.xyz);
    let _skTemp3 = dFdy(_globalUniforms.testInputs);
    let _skTemp4 = dFdy(coords.xx);
    let _skTemp5 = sign(_skTemp4);
    let _skTemp6 = dFdy(coords.yy);
    let _skTemp7 = sign(_skTemp6);
    let _skTemp8 = dFdy(coords);
    let _skTemp9 = sign(_skTemp8);
    return select(_globalUniforms.colorRed, _globalUniforms.colorGreen, vec4<bool>((((((_skTemp0 == expected.x && all(_skTemp1 == expected.xy)) && all(_skTemp2 == expected.xyz)) && all(_skTemp3 == expected)) && all(_skTemp5 == vec2<f32>(0.0))) && all(_skTemp7 == vec2<f32>(1.0))) && all(_skTemp9 == vec2<f32>(0.0, 1.0))));
  }
}
@fragment fn fragmentMain(_stageIn: FSIn) -> FSOut {
  var _stageOut: FSOut;
  _stageOut.sk_FragColor = main(_stageIn.sk_FragCoord.xy);
  return _stageOut;
}

1 error