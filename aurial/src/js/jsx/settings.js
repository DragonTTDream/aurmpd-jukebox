import { h, Component } from 'preact';
import {APP_VERSION, UPSTREAM_URL, UPSTREAM_LABEL, REPO_URL, REPO_LABEL, LICENSE_NAME} from '../version'
import Subsonic from '../subsonic'
import {UniqueID} from '../util'
import {Messages} from './app'
import {Prompt} from './common'
import {t, getLanguage, setLanguage, languages} from '../i18n'
// 未识别类型的 WS 消息由 player.js 广播为 "mpdMessage" 事件（见文件头注释），
// 自动启停开关通过它接收 {"type":"autostart", ...} 回包
import {sendCommand} from '../mpdws'

const TEST_UNTESTED = 0;
const TEST_BUSY = 1;
const TEST_SUCCESS = 2;
const TEST_FAILED = 3;

export default class Settings extends Component {

	state = {
		url: this.props.subsonic.url,
		user: this.props.subsonic.user,
		password: '',
		notifications: localStorage.getItem('notifications') === 'true',
		backgroundArt: localStorage.getItem('backgroundArt') === 'true',
		trackBuffer: localStorage.getItem('trackBuffer') || '0',
		testState: TEST_UNTESTED,
		lang: getLanguage(),
		// 开机自启由服务端说了算：supported=null 表示还没问到（先不渲染）
		autostartSupported: null,
		autostartEnabled: false,
		autostartPending: false,
		// 音频输出（扬声器）同样以服务端为准：supported=null 表示还没问到（先不渲染）
		audioSupported: null,
		audioCanSet: false,
		audioCurrent: '',
		audioDevices: [],
		audioPending: false
	};

	constructor(props, context) {
		super(props, context);

		this.save = this.save.bind(this);
		this.change = this.change.bind(this);
		this.demo = this.demo.bind(this);
		this.test = this.test.bind(this);
		this.changeLanguage = this.changeLanguage.bind(this);
		this.receive = this.receive.bind(this);
		this.toggleAutostart = this.toggleAutostart.bind(this);
		this.queryAutostart = this.queryAutostart.bind(this);
		this.queryAudioDevices = this.queryAudioDevices.bind(this);
		this.changeAudioDevice = this.changeAudioDevice.bind(this);
		this.refreshAudio = this.refreshAudio.bind(this);
		this.applyAudioDevice = this.applyAudioDevice.bind(this);

		props.events.subscribe({
			subscriber: this,
			event: ["mpdMessage"]
		});
	}

	componentDidMount() {
		// 设置页在应用启动时就已挂载（标签页只是被 CSS 隐藏），所以这里先查一次，
		// 再在每次点开设置标签时重查，保证看到的是服务端当前值
		this.queryAutostart();
		this.queryAudioDevices();
		this.autostartTab = document.querySelector('a.item[data-tab="settings"]');
		if (this.autostartTab) {
			this.autostartTabHandler = () => { this.queryAutostart(); this.queryAudioDevices(); };
			this.autostartTab.addEventListener('click', this.autostartTabHandler);
		}
	}

	componentWillUnmount() {
		this.clearAutostartTimer();
		this.clearAudioTimer();
		if (this.autostartTab && this.autostartTabHandler) {
			this.autostartTab.removeEventListener('click', this.autostartTabHandler);
		}
	}

	clearAutostartTimer() {
		if (this.autostartTimer) {
			clearTimeout(this.autostartTimer);
			this.autostartTimer = null;
		}
	}

	queryAutostart() {
		sendCommand('MPD_API_GET_AUTOSTART');
	}

	receive(event) {
		if (event.event == "mpdMessage") this.mpdMessage(event.data);
	}

	// 服务端回包：{"type":"autostart","data":{"supported":bool,"enabled":bool}}
	//            {"type":"audio_devices","data":{...}}
	mpdMessage(msg) {
		if (!msg) return;
		if (msg.type == 'autostart' && msg.data) {
			this.clearAutostartTimer();
			// 以服务端回包校正 UI（SET 之后也走这里）
			this.setState({
				autostartSupported: !!msg.data.supported,
				autostartEnabled: !!msg.data.enabled,
				autostartPending: false
			});
		} else if (msg.type == 'audio_devices' && msg.data) {
			this.audioMessage(msg.data);
		} else if (msg.type == 'error' && this.state.audioPending) {
			// 应用失败：回显服务端给的原因，不静默
			this.clearAudioTimer();
			this.setState({audioPending: false});
			Messages.message(this.props.events,
				t('settings.audioFailedDetail', {error: msg.data}), "error", "warning sign");
		}
	}

	// ---- 音频输出设备 ----

	clearAudioTimer() {
		if (this.audioTimer) {
			clearTimeout(this.audioTimer);
			this.audioTimer = null;
		}
	}

	queryAudioDevices() {
		sendCommand('MPD_API_GET_AUDIO_DEVICES');
	}

	changeAudioDevice(e) {
		this.setState({audioCurrent: e.target.value});
	}

	refreshAudio(e) {
		if (e) e.preventDefault();
		this.queryAudioDevices();
	}

	applyAudioDevice(e) {
		if (e) e.preventDefault();
		if (!this.state.audioCanSet || this.state.audioPending) return;

		this.setState({audioPending: true});
		sendCommand('MPD_API_SET_AUDIO_DEVICE,' + this.state.audioCurrent);

		// 没有回包/失败时不静默：给可见提示
		this.clearAudioTimer();
		this.audioTimer = setTimeout(function() {
			this.audioTimer = null;
			this.setState({audioPending: false});
			Messages.message(this.props.events, t('settings.audioFailed'), "error", "warning sign");
		}.bind(this), 20000);
	}

	// 服务端回包：{"type":"audio_devices","data":{supported,current,canSet,devices[]}}
	audioMessage(data) {
		var devices = Array.isArray(data.devices) ? data.devices : [];
		var current = data.current || '';
		var known = devices.some(function(d) { return d.id === current; });
		var wasPending = this.state.audioPending;

		if (!known && devices.length) current = devices[0].id;

		this.clearAudioTimer();
		this.setState({
			audioSupported: !!data.supported,
			audioCanSet: !!data.canSet,
			audioDevices: devices,
			audioCurrent: current,
			audioPending: false
		});

		if (wasPending) {
			// 这是「应用」的回包：给反馈，并按需求重新查询一次校准实际状态
			Messages.message(this.props.events, t('settings.audioApplied'), "success", "volume up");
			this.queryAudioDevices();
		}
	}

	toggleAutostart(e) {
		var next = e.target.checked;
		// 先按点击结果显示，等服务端回包再校正
		this.setState({autostartEnabled: next, autostartPending: true});
		sendCommand('MPD_API_SET_AUTOSTART,' + (next ? 1 : 0));

		// 没有回包/失败时不静默：给可见提示并回到原值
		this.clearAutostartTimer();
		this.autostartTimer = setTimeout(function() {
			this.autostartTimer = null;
			this.setState({autostartEnabled: !next, autostartPending: false});
			Messages.message(this.props.events, t('settings.autostartFailed'), "error", "warning sign");
		}.bind(this), 5000);
	}

	// Language switches instantly (no Save needed) and is persisted by i18n.
	changeLanguage(e) {
		var lang = setLanguage(e.target.value);
		this.setState({lang: lang});
	}

	save(e) {
		e.preventDefault();

		localStorage.setItem('url', this.state.url);
		localStorage.setItem('username', this.state.user);

		if (this.state.password !== '') {
			var salt = UniqueID();
			localStorage.setItem('token', Subsonic.createToken(this.state.password, salt));
			localStorage.setItem('salt', salt);
		}

		localStorage.setItem('notifications', this.state.notifications);
		localStorage.setItem('backgroundArt', this.state.backgroundArt);
		localStorage.setItem('trackBuffer', this.state.trackBuffer);

		Messages.message(this.props.events, t('settings.saved'), "success", "Save");

		// reload app with new settings
		var subsonic = new Subsonic(
			localStorage.getItem('url'),
			localStorage.getItem('username'),
			localStorage.getItem('token'),
			localStorage.getItem('salt'),
			this.props.subsonic.version,
			this.props.subsonic.appName
		);

		// publish new settings to negate need to reload the page - App consumes these
		this.props.events.publish({event: "appSettings",
			data: {
				subsonic: subsonic,
				trackBuffer: localStorage.getItem('trackBuffer')
			}
		});
	}

	demo(e) {
		e.preventDefault();
		this.demoPrompt.show(function(approve) {
			if (!approve) return;

			this.setState({
				url: "http://demo.subsonic.org",
				user: "guest",
				password: "guest"
			});

		}.bind(this));
	}

	test(e) {
		e.preventDefault();

		var salt = UniqueID();

		var subsonic = new Subsonic(
			this.state.url,
			this.state.user,
			Subsonic.createToken(this.state.password, salt),
			salt,
			this.props.subsonic.version,
			this.props.subsonic.appName
		);

		this.setState({testState: TEST_BUSY});

		subsonic.ping({
			success: function(data) {
				if (data.status === "ok") {
					this.setState({testState: TEST_SUCCESS});
					Messages.message(this.props.events, t('settings.testSuccess'), "success", "plug");
				} else {
					console.log(data.error);
					this.setState({testState: TEST_FAILED});
					Messages.message(this.props.events, data.error.message, "error", "plug");
				}
			}.bind(this),
			error: function(err) {
				this.setState({testState: TEST_FAILED});
				Messages.message(this.props.events, t('settings.testFailed', {error: err.message}), "error", "plug");
			}.bind(this)
		});
	}

	change(e) {
		switch (e.target.name) {
			case "url": this.setState({url: e.target.value}); break;
			case "user": this.setState({user: e.target.value}); break;
			case "password": this.setState({password: e.target.value}); break;
			case "notifications": this.setState({notifications: e.target.checked}); break;
			case "backgroundArt": this.setState({backgroundArt: e.target.checked}); break;
			case "trackBuffer": this.setState({trackBuffer: e.target.value}); break;
		}

		this.setState({testState: TEST_UNTESTED});
	}

	render() {
		var testIcon = "circle thin";
		switch (this.state.testState) {
			case TEST_BUSY: testIcon = "loading spinner"; break;
			case TEST_SUCCESS: testIcon = "green checkmark"; break;
			case TEST_FAILED: testIcon = "red warning sign"; break;
			default: testIcon = "circle thin";
		}

		return (
			<div className="ui basic segment">
				<form className="ui form" onSubmit={this.save}>
					<h3 className="ui dividing header">
						{t('settings.language')}
					</h3>
					<div className="field">
						<select name="lang" onChange={this.changeLanguage} value={this.state.lang}>
							{languages.map(function(language) {
								return <option key={language.code} value={language.code}>{language.label}</option>;
							})}
						</select>
					</div>

					<h3 className="ui dividing header">
						{t('settings.connection')}
					</h3>
					<div className="field">
						<label>{t('settings.url')}</label>
						<input name="url" placeholder="http://yourname.subsonic.com" type="text" onChange={this.change} value={this.state.url} />
					</div>
					<div className="two fields">
						<div className="field">
							<label>{t('settings.username')}</label>
							<input name="user" placeholder="username" type="text" onChange={this.change} value={this.state.user} />
						</div>
						<div className="field">
							<label>{t('settings.password')} (<i>{t('settings.passwordHint')}</i>)</label>
							<input name="password" placeholder="password" type="password" onChange={this.change} value={this.state.password} />
						</div>
					</div>

					<h3 className="ui dividing header">
						{t('settings.preferences')}
					</h3>
					<div className="field">
						<label>{t('settings.bufferLabel')}</label>
						<select name="trackBuffer" onChange={this.change} value={this.state.trackBuffer}>
							<option value="0">{t('settings.bufferDisabled')}</option>
							<option value="10">{t('settings.buffer10')}</option>
							<option value="30">{t('settings.buffer30')}</option>
						</select>
					</div>
					<div className="field">
						<div className="ui checkbox">
							<input name="notifications" type="checkbox" onChange={this.change} checked={this.state.notifications}/>
							<label>{t('settings.notifications')}</label>
						</div>
					</div>
					<div className="field">
						<div className="ui checkbox">
							<input name="backgroundArt" type="checkbox" onChange={this.change} checked={this.state.backgroundArt}/>
							<label>{t('settings.backgroundArt')}</label>
						</div>
					</div>
					{/* supported=false（例如 Linux 版）或还没问到回包时，这一项完全不渲染 */}
					{this.state.autostartSupported === true ? (
						<div className="field autostart-field">
							<div className={'ui checkbox' + (this.state.autostartPending ? ' autostart-pending' : '')}>
								<input name="autostart" type="checkbox" onChange={this.toggleAutostart} checked={this.state.autostartEnabled}/>
								<label>{t('settings.autostart')}</label>
							</div>
						</div>
					) : null}

					{/* 音频输出设备：supported=false（未检测到设备）时整块不渲染，与「开机自启」一致；
					    canSet=false（例如 Linux）时禁用「应用」并提示手改 mpd.conf */}
					{this.state.audioSupported === true ? (
						<div className="field audio-output-field">
							<h3 className="ui dividing header">{t('settings.audioOutput')}</h3>
							<div className="audio-output-controls">
								<select className="audio-device-select" name="audioDevice" onChange={this.changeAudioDevice} value={this.state.audioCurrent} disabled={!this.state.audioCanSet}>
									{this.state.audioDevices.map(function(device) {
										return <option key={device.id} value={device.id}>{device.name}</option>;
									})}
								</select>
								<button className="ui button audio-refresh-button" type="button" onClick={this.refreshAudio} disabled={this.state.audioPending}>{t('settings.audioRefresh')}</button>
								<button className="ui blue button audio-apply-button" type="button" onClick={this.applyAudioDevice} disabled={!this.state.audioCanSet || this.state.audioPending}>{t('settings.audioApply')}</button>
							</div>
							{this.state.audioCanSet ? null : (
								<div className="ui pointing label audio-output-hint">{t('settings.audioManualHint')}</div>
							)}
						</div>
					) : null}

					<div className="ui section divider"></div>

					<h3 className="ui dividing header">
						{t('settings.about')}
					</h3>
					<div className="ui inverted segment about-box">
						<p><strong>{t('settings.basedOn')}:</strong> <a href={UPSTREAM_URL} target="_blank" rel="noopener">{UPSTREAM_LABEL}</a> <span className="repo-note">({t('app.upstream')})</span></p>
						<p><strong>{t('app.thisRepo')}:</strong> <a href={REPO_URL} target="_blank" rel="noopener">{REPO_LABEL}</a></p>
						<p><strong>{t('settings.version')}:</strong> {APP_VERSION} &nbsp;·&nbsp; <strong>{t('settings.license')}:</strong> {LICENSE_NAME}</p>
						<p className="repo-note">{t('settings.repoNote')}</p>
					</div>
										<button className="ui blue button" type="submit">{t('settings.save')}</button>
					<button className="ui button" onClick={this.demo}>{t('settings.demo')}</button>
					<button className="ui icon button" onClick={this.test}>
						<i className={testIcon + " icon"}></i>
						{t('settings.test')}
					</button>
				</form>

				<Prompt ref={(r) => {this.demoPrompt = r;} } title={t('settings.demoTitle')}
					message={t('settings.demoMessage')}
					ok={t('settings.yes')} cancel={t('settings.no')} icon="red question" />
			</div>
		);
	}
}
