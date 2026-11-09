import { UIAbility, Want, AbilityConstant } from '@kit.AbilityKit';
import { window } from '@kit.ArkUI';
import { AudioSession } from '../AudioSession';

export default class EntryAbility extends UIAbility {
  private pendingWant?: Want;
  private windowReady: boolean = false;
  private destroyed: boolean = false;
  private operations: Promise<void> = Promise.resolve();

  onCreate(want: Want, _launchParam: AbilityConstant.LaunchParam): void {
    this.destroyed = false;
    AudioSession.shared.attach(this.context);
    this.pendingWant = want;
  }
  onWindowStageCreate(stage: window.WindowStage): void {
    stage.loadContent('pages/Index', (error) => {
      if (error.code) return;
      this.windowReady = true;
      if (this.pendingWant) this.enqueue(this.pendingWant);
      this.pendingWant = undefined;
    });
  }
  onNewWant(want: Want, _launchParam: AbilityConstant.LaunchParam): void {
    if (this.windowReady) this.enqueue(want);
    else this.pendingWant = want;
  }
  private enqueue(want: Want): void {
    this.operations = this.operations.then(() => {
      if (!this.destroyed) return AudioSession.shared.handle(want);
      return Promise.resolve();
    }).catch(() => {
      AudioSession.shared.stop('音频启动失败，请重新连接电脑');
    });
  }
  onDestroy(): void {
    this.destroyed = true;
    this.windowReady = false;
    AudioSession.shared.stop();
  }
}
