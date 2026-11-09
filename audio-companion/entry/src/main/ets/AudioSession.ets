import { common, Want, wantAgent } from '@kit.AbilityKit';
import { backgroundTaskManager } from '@kit.BackgroundTasksKit';
import capture from 'libaudio_capture.so';

export class AudioSession {
  static shared: AudioSession = new AudioSession();
  private context?: common.UIAbilityContext;
  private session: string = '';
  private timer: number = -1;
  private background: boolean = false;
  private requestingBackground: boolean = false;
  private backgroundAttempted: boolean = false;
  private backgroundFailed: boolean = false;
  private backgroundPromise?: Promise<void>;
  private generation: number = 0;

  attach(context: common.UIAbilityContext): void {
    this.context = context;
    AppStorage.setOrCreate<string>('audioStatus', '等待电脑启动音频转发');
    AppStorage.setOrCreate<boolean>('audioActive', false);
  }

  async handle(want: Want): Promise<void> {
    const action = want.parameters?.['action'];
    const session = want.parameters?.['session'];
    if (typeof session !== 'string' || !/^[a-f0-9]{32}$/.test(session)) return;
    if (action === 'stop') {
      if (session === this.session) {
        await this.stop();
        try { await this.context?.terminateSelf(); } catch { }
      }
      return;
    }
    const port = want.parameters?.['port'];
    if (action !== 'start' || typeof port !== 'number' || port < 1 || port > 65535 ||
      Math.floor(port) !== port) return;
    if (session === this.session && AppStorage.get<boolean>('audioActive')) return;
    const stopGeneration = this.generation + 1;
    await this.stop();
    if (this.generation !== stopGeneration) return;
    this.session = session;
    if (!capture.start(port, session)) {
      AppStorage.set<string>('audioStatus', capture.status());
      return;
    }
    AppStorage.set<boolean>('audioActive', true);
    AppStorage.set<string>('audioStatus', '请在系统弹窗中允许采集手机声音');
    const generation = this.generation;
    this.timer = setInterval(() => { this.refresh(generation); }, 500);
  }

  private refresh(generation: number): void {
    if (generation !== this.generation) return;
    const status = capture.status();
    if (status === 'capturing') {
      AppStorage.set<string>('audioStatus', this.backgroundFailed ?
        '正在传输；后台运行未获允许，请保持此页在前台' : '正在向电脑传输手机声音');
      if (!this.background && !this.requestingBackground && !this.backgroundAttempted) {
        this.requestingBackground = true;
        this.backgroundAttempted = true;
        this.backgroundPromise = this.startBackground(generation);
      }
    } else if (status === 'private') {
      AppStorage.set<string>('audioStatus', '隐私场景中，音频传输已暂停');
    } else if (status === 'stopped' || status.startsWith('error:')) {
      this.stop(status === 'stopped' ? '音频传输已停止' : status);
    }
  }

  private async startBackground(generation: number): Promise<void> {
    const context = this.context;
    if (!context) return;
    try {
      const agent = await wantAgent.getWantAgent({
        wants: [{ bundleName: 'com.hscrcpy.audiocapture', abilityName: 'EntryAbility' }],
        actionType: wantAgent.OperationType.START_ABILITY,
        requestCode: 0,
        actionFlags: [wantAgent.WantAgentFlags.UPDATE_PRESENT_FLAG]
      });
      if (generation !== this.generation) return;
      await backgroundTaskManager.startBackgroundRunning(
        context, backgroundTaskManager.BackgroundMode.AUDIO_RECORDING, agent);
      if (generation !== this.generation) {
        await backgroundTaskManager.stopBackgroundRunning(context);
        return;
      }
      this.background = true;
      await context.moveAbilityToBackground();
    } catch {
      if (generation === this.generation) {
        this.backgroundFailed = true;
        AppStorage.set<string>('audioStatus', '正在传输；后台运行未获允许，请保持此页在前台');
      }
    } finally {
      if (generation === this.generation) this.requestingBackground = false;
    }
  }

  async stop(message: string = '音频传输已停止'): Promise<void> {
    const generation = ++this.generation;
    if (this.timer !== -1) { clearInterval(this.timer); this.timer = -1; }
    capture.stop();
    const pendingBackground = this.backgroundPromise;
    if (pendingBackground) await pendingBackground;
    if (generation !== this.generation) return;
    this.backgroundPromise = undefined;
    const wasBackground = this.background;
    this.background = false;
    this.requestingBackground = false;
    this.backgroundAttempted = false;
    this.backgroundFailed = false;
    this.session = '';
    AppStorage.set<boolean>('audioActive', false);
    AppStorage.set<string>('audioStatus', message);
    if (wasBackground && this.context) {
      try { await backgroundTaskManager.stopBackgroundRunning(this.context); } catch { }
    }
  }
}
