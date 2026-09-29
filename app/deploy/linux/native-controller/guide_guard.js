// Stream-only local Steam menu guard. A short lease restores normal input even
// if Moonlight or the helper dies. Never suppress another controller's guide.
(() => {
  if (window.__moonmachineGuideGuard) return { installed: true };
  const sources = FocusNavController.m_rgGamepadInputSources.filter(
    s => typeof s.OnSystemButtonPress === 'function');
  if (!sources.length) throw new Error('Steam system-button source not found');
  const state = { deadline: performance.now() + 3000, intercepted: 0, index: null, sources: [], timer: null };
  state.restore = () => {
    clearInterval(state.timer);
    for (const entry of state.sources) {
      if (entry.source.OnSystemButtonPress === entry.wrapper) {
        if (entry.own) entry.source.OnSystemButtonPress = entry.original;
        else delete entry.source.OnSystemButtonPress;
      }
    }
    delete window.__moonmachineGuideGuard;
  };
  for (const source of sources) {
    const original = source.OnSystemButtonPress;
    const wrapper = function(button, index) {
      const controller = ControllerStore.GetController(index);
      const type = controller && ControllerStore.GetControllerTypeString(controller.eControllerType);
      if (button === 27 && type === 'controller_steamcontroller_triton' && performance.now() < state.deadline) {
        state.index = index;
        ++state.intercepted;
        return;
      }
      return original.call(this, button, index);
    };
    state.sources.push({ source, original, wrapper, own: Object.prototype.hasOwnProperty.call(source, 'OnSystemButtonPress') });
    source.OnSystemButtonPress = wrapper;
  }
  state.local = () => {
    const controllers = ControllerStore.GetControllers();
    const controller = controllers.find(c => c.nControllerIndex === state.index &&
      ControllerStore.GetControllerTypeString(c.eControllerType) === 'controller_steamcontroller_triton') ||
      controllers.find(c => ControllerStore.GetControllerTypeString(c.eControllerType) === 'controller_steamcontroller_triton');
    if (!controller) return false;
    const entry = state.sources[0];
    entry.original.call(entry.source, 27, controller.nControllerIndex);
    return true;
  };
  state.timer = setInterval(() => { if (performance.now() >= state.deadline) state.restore(); }, 500);
  window.__moonmachineGuideGuard = state;
  return { installed: true };
})()
