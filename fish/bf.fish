function bf --description "Babyface card profile: bf off | bf on | bf <profile>"
    set -l card (pactl list cards short | awk '/RME_Babyface/ {print $2}' | head -1)
    if test -z "$card"
        echo "bf: no Babyface in pactl - is snd_usb_babyface_pro loaded?" >&2
        return 1
    end

    switch "$argv[1]"
        case off
            # Free the card so ratesweep/ratecheck/ratesteal can have it.
            pactl set-card-profile $card off
            and echo "bf: $card -> off"
        case on '' pro-audio
            pactl set-card-profile $card pro-audio
            and echo "bf: $card -> pro-audio"
        case profiles
            pactl list cards | grep -A 40 "Name: $card" | sed -n '/Profiles:/,/Active Profile:/p'
        case status
            pactl list cards | grep -A 40 "Name: $card" | grep 'Active Profile:'
        case '*'
            pactl set-card-profile $card $argv[1]
            and echo "bf: $card -> $argv[1]"
    end
end
