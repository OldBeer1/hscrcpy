import { AudioSession } from '../AudioSession';

@Entry
@Component
struct Index {
  @StorageLink('audioStatus') status: string = '等待电脑启动音频转发';
  @StorageLink('audioActive') active: boolean = false;

  build() {
    Column({ space: 24 }) {
      Text('hscrcpy 音频').fontSize(28).fontWeight(FontWeight.Bold)
      Text(this.status).fontSize(18).textAlign(TextAlign.Center)
      Text('只采集手机内部播放的声音，不采集麦克风。开始时需要确认系统授权，可随时停止。')
        .fontSize(15).fontColor('#666666').textAlign(TextAlign.Center)
      Button('停止传输').enabled(this.active).onClick(() => { AudioSession.shared.stop(); })
    }.width('100%').height('100%').padding(28).justifyContent(FlexAlign.Center)
  }
}
